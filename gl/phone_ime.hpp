// Type from the phone: a tiny local HTTP server so the user can type on
// their mobile browser instead of the on-screen keyboard. Runs entirely on
// its own thread; the GL thread only polls for finished texts.
// The OS IME dialog (ime.hpp) stays the fallback and is unaffected.

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sx
{

class PhoneIme
{
  public:
    PhoneIme() = default;
    ~PhoneIme()
    {
        stop();
    }
    PhoneIme(const PhoneIme &) = delete;
    PhoneIme &operator=(const PhoneIme &) = delete;

    // Bind a LAN IPv4 address on the first free port in 8765..8775 and start
    // serving. False when no address/port worked: the caller falls back to
    // the OS IME. Safe to call when already running (no-op).
    bool start();
    // Stop serving, close the socket, join the thread. Safe when stopped.
    void stop();
    bool running() const
    {
        return running_.load();
    }
    // "http://192.168.1.20:8765": shown as a QR card while typing. Empty
    // when not running.
    std::string url() const;
    // Drain one received text (main thread). False when the queue is empty.
    bool poll(std::string *out);

  private:
    void serve();
    // One HTTP exchange on an accepted client fd. Returns the POSTed text
    // (empty when none).
    std::string handle_client(int fd);

    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::string url_;
    std::vector<std::string> inbox_; // received texts, main thread drains
    int listen_fd_ = -1;
};

} // namespace sx
