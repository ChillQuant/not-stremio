// Type from the phone: local HTTP listener on the LAN.
//
// The worker thread owns the listen socket. Each accepted client is handled
// inline (one at a time is plenty for a typing page) with a small select()
// timeout so stop() never hangs. Requests are capped at 4096 bytes; anything
// bigger is dropped. Only two routes exist:
//   GET  /      -> the mobile typing page
//   POST /text  -> body is "text=<url-encoded UTF-8>", queued for the UI
// Everything else gets a 404. No auth: this is a typing helper on the LAN,
// and it only runs while a text field is open.

#include "phone_ime.hpp"

#include "../src/util.h" // lower() for case-insensitive Content-Length lookup

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace sx
{

namespace
{

constexpr int kPortFirst = 8765;
constexpr int kPortLast = 8775;
constexpr std::size_t kMaxRequest = 4096;

std::string lan_ipv4()
{
    // First non-loopback IPv4 address: the phone is on the same LAN.
    ifaddrs *addrs = nullptr;
    if (getifaddrs(&addrs) != 0)
        return {};
    std::string ip;
    for (ifaddrs *a = addrs; a; a = a->ifa_next)
    {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
            continue;
        char buf[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr, buf, sizeof buf))
            continue;
        if (std::strncmp(buf, "127.", 4) == 0)
            continue;
        ip = buf;
        break;
    }
    freeifaddrs(addrs);
    return ip;
}

std::string url_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            unsigned v = 0;
            if (std::sscanf(s.c_str() + i + 1, "%2x", &v) == 1)
            {
                out.push_back(static_cast<char>(v));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

const char kPage[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Type for Stremio</title>"
    "<style>body{background:#0c0b11;color:#eee;font-family:sans-serif;"
    "display:flex;flex-direction:column;align-items:center;justify-content:center;"
    "height:100vh;margin:0}input{font-size:22px;padding:12px;width:80%;"
    "border-radius:10px;border:1px solid #3a2a8f;background:#14111f;color:#fff}"
    "button{font-size:20px;margin-top:14px;padding:12px 40px;border-radius:24px;"
    "border:0;background:#6d5ef0;color:#fff}</style></head>"
    "<body><h2>Type for Stremio</h2>"
    "<form method=post action=/text><input name=text autofocus autocomplete=off>"
    "<br><button>Send</button></form></body></html>";

void send_all(int fd, const char *p, std::size_t n)
{
    while (n > 0)
    {
        const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

void reply(int fd, const char *status, const char *ctype, const char *body, std::size_t len)
{
    char head[256];
    const int hlen =
        std::snprintf(head, sizeof head, "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                                         "Connection: close\r\n\r\n",
                      status, ctype, len);
    send_all(fd, head, static_cast<std::size_t>(hlen));
    send_all(fd, body, len);
}

} // namespace

bool PhoneIme::start()
{
    if (running_.load())
        return true;
    // A previous run may have left a finished-but-unjoined thread behind
    // (serve() exits on its own when the listen fd goes away): overwriting a
    // joinable std::thread with a new one would std::terminate.
    if (thread_.joinable())
        thread_.join();
    const std::string ip = lan_ipv4();
    if (ip.empty())
        return false;
    int fd = -1;
    int port = 0;
    for (int p = kPortFirst; p <= kPortLast; ++p)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(p));
        ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 &&
            ::listen(fd, 4) == 0)
        {
            port = p;
            break;
        }
        ::close(fd);
        fd = -1;
    }
    if (fd < 0)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listen_fd_ = fd;
        url_ = "http://" + ip + ":" + std::to_string(port);
        inbox_.clear();
    }
    running_.store(true);
    thread_ = std::thread(&PhoneIme::serve, this);
    return true;
}

void PhoneIme::stop()
{
    if (!running_.exchange(false))
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (listen_fd_ >= 0)
        {
            // Shutting the fd down wakes the select() in serve().
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        url_.clear();
    }
    if (thread_.joinable())
        thread_.join();
}

std::string PhoneIme::url() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

bool PhoneIme::poll(std::string *out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inbox_.empty())
        return false;
    *out = std::move(inbox_.front());
    inbox_.erase(inbox_.begin());
    return true;
}

void PhoneIme::serve()
{
    while (running_.load())
    {
        int lfd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lfd = listen_fd_;
        }
        if (lfd < 0)
            break;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        timeval tv{0, 200000}; // 200 ms: stop() never waits long
        const int r = ::select(lfd + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0)
            continue;
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0)
            continue;
        const std::string text = handle_client(cfd);
        ::close(cfd);
        if (!text.empty())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(text);
        }
    }
}

std::string PhoneIme::handle_client(int fd)
{
    // Read the head first, then exactly Content-Length body bytes; the whole
    // request is capped at 4096 bytes.
    std::string req;
    req.reserve(1024);
    char buf[512];
    std::size_t body_at = std::string::npos;
    std::size_t content_len = 0;
    bool is_post_text = false;
    for (;;)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{2, 0};
        if (::select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0)
            return {};
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0)
            return {};
        if (req.size() + static_cast<std::size_t>(n) > kMaxRequest)
            return {}; // too big: drop it
        req.append(buf, static_cast<std::size_t>(n));
        if (body_at == std::string::npos)
        {
            const std::size_t h = req.find("\r\n\r\n");
            if (h == std::string::npos)
                continue;
            body_at = h + 4;
            is_post_text = req.compare(0, 10, "POST /text") == 0;
            const std::size_t cl = lower(req.substr(0, body_at)).find("content-length:");
            if (cl != std::string::npos && cl < body_at)
                content_len = static_cast<std::size_t>(std::strtoul(req.c_str() + cl + 15, nullptr, 10));
            if (!is_post_text || content_len == 0 || content_len > kMaxRequest)
                break;
        }
        if (req.size() >= body_at + content_len)
            break;
    }
    if (req.compare(0, 5, "GET /") == 0 && req.find("HTTP/") != std::string::npos)
    {
        reply(fd, "200 OK", "text/html; charset=utf-8", kPage, sizeof kPage - 1);
        return {};
    }
    if (is_post_text)
    {
        const std::string body = req.substr(body_at, content_len);
        // form: text=<url-encoded>
        std::string text;
        const std::size_t eq = body.find('=');
        if (eq != std::string::npos && body.compare(0, eq, "text") == 0)
            text = url_decode(body.substr(eq + 1));
        const char ok[] = "sent";
        reply(fd, "200 OK", "text/plain; charset=utf-8", ok, sizeof ok - 1);
        return text;
    }
    const char nf[] = "not found";
    reply(fd, "404 Not Found", "text/plain", nf, sizeof nf - 1);
    return {};
}

} // namespace sx
