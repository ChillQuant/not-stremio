// Stremio for the PS5 on the OpenGL toolkit: opens the display and the
// controller, then runs the frame loop. What the buttons mean is ui/shell.cpp,
// what the screens show is gl/session.cpp.

#include "chrome.hpp"
#include "core/input.hpp"
#include "dynamic_font.hpp"
#include "gfx/renderer.hpp"
#include "hdr_probe.h"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "overlays.hpp"
#include "page.hpp"
#include "session.hpp"
#include "shell.hpp"
#include "theme.hpp"
#include "../src/util.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <span>
#include <string>

extern "C" void ps5_load_modules(void); // the keyboard dialog's module (native/ps5_modules.c)

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

namespace
{

constexpr const char *kRoot = "/app0";
// The title's own storage (param.json downloadDataSize): settings, progress, artwork.
constexpr const char *kData = "/download0/stremio";

struct Mode
{
    int width, height;
};

// 4K first, then what a 1080p television can show.
constexpr Mode kModes[] = {{3840, 2160}, {1920, 1080}};

std::uint32_t load_icon(hui::gfx::Renderer &renderer, const std::string &path)
{
    int w = 0, h = 0, comp = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!pixels)
    {
        hui::sys::log("[SX] cannot load %s", path.c_str());
        return 0;
    }
    const std::uint32_t texture = renderer.batch().create_texture(w, h, pixels);
    stbi_image_free(pixels);
    return texture;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[SX] entry");
    const std::string root = kRoot;
    // A closed connection must fail a write quietly instead of killing us.
    std::signal(SIGPIPE, SIG_IGN);

    ps5::Display display;
    bool opened = false;
    for (const Mode &mode : kModes)
    {
        if (display.open(mode.width, mode.height))
        {
            opened = true;
            sys::log("[SX] display %dx%d", display.width(), display.height());
            break;
        }
        sys::log("[SX] display %dx%d failed", mode.width, mode.height);
    }
    if (!opened)
        sys::park();

    gfx::Renderer renderer;
    static sx::UiFonts ui_fonts;
    static ui::Fonts fonts;
    if (!renderer.init())
    {
        sys::log("[SX] fatal: renderer failed");
        sys::park();
    }

    // STEP 1 (Instant Board): the boot splash loop is gone. The console splash
    // is hidden right after the first Board frame is presented by the main loop
    // below, so the first picture the user sees is already the interactive
    // Board. The removed frames did nothing the main loop does not already do
    // (no event pumping, no watchdog) apart from drawing the logo and hiding
    // the splash; the splash-hide is preserved in the main loop.
    static sx::Icons icons;
    const std::string icon_dir = root + "/icons";
    icons.logo_xl = load_icon(renderer, icon_dir + "/logo_xl.png");
    icons.name = load_icon(renderer, icon_dir + "/name.png");
    const std::int64_t boot_t0 = sys::monotonic_us();
    const auto boot_seconds = [&] { return static_cast<float>(sys::monotonic_us() - boot_t0) / 1e6f; };
    bool splash_hidden = false;
    ps5_load_modules(); // the keyboard dialog
    // (The hardware video decoder module used to load here; it now loads on
    // the task pool in Session::init, next to the addon fetches, and is only
    // needed at the first playback anyway.)
    if (!sx::load_ui_fonts(renderer, root + "/fonts", root + "/hui-fonts", &ui_fonts, &fonts))
    {
        sys::log("[SX] fatal: fonts failed");
        sys::park();
    }

    const char *solid[5] = {"nav_board", "nav_discover", "nav_library", "nav_addons", "nav_settings"};
    for (int i = 0; i < 5; ++i)
    {
        icons.nav_solid[i] = load_icon(renderer, icon_dir + "/" + solid[i] + ".png");
        icons.nav_line[i] = load_icon(renderer, icon_dir + "/" + solid[i] + "_o.png");
    }
    icons.search = load_icon(renderer, icon_dir + "/search.png");
    icons.logo = load_icon(renderer, icon_dir + "/logo_l.png");

    static sx::Session session;
    if (!session.init(root, kData, &renderer))
    {
        sys::log("[SX] fatal: session failed");
        sys::park();
    }
    // HDR10 diagnostic probe (Settings, default OFF): a read-only VideoOut
    // status query that logs what the HDMI output reports. Never changes the
    // picture; runs once here because prefs are loaded by session.init().
    if (session.hdr_probe_enabled())
        hdr_probe::hdr_signal_probe(display.width(), display.height());
    // One-time warning if Thai fonts are missing: Thai subtitles will render
    // as '?' without them. The dlog in load_ui_fonts has the details.
    if (!ui_fonts.missing_thai_fonts.empty())
    {
        std::string names;
        for (const std::string &f : ui_fonts.missing_thai_fonts)
        {
            if (!names.empty())
                names += ", ";
            names += f;
        }
        session.say("Missing Thai fonts (" + names + ") - Thai subtitles may show as '?'.", 8.0, true);
    }
    // The display's real size: the player may emit 4K video frames when the
    // GPU conversion path is active and the display is a 2160p mode.
    session.set_display_size(display.width(), display.height());
    static sx::Env env{fonts, icons};
    static sx::Shell shell(env, session);
    shell.start_sound(root + "/sounds"); // the interface's sounds
    shell.start_boot_at(boot_seconds()); // the animation goes on from where the frames above left it

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    PadSample samples[64];

    std::uint64_t frames = 0;
    std::int64_t last_start = sys::monotonic_us();
    std::int64_t window_start = last_start;
    int window_frames = 0;
    float worst_ms = 0.0f;
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_start) / 1e6f;
        last_start = now;
        const float frame_ms = dt * 1000.0f;
        dt = std::min(dt, 0.05f); // a hitch must not teleport the animations

        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count), static_cast<std::uint64_t>(now));
        session.update(dt, shell.cursor());
        env.clock = session.clock;
        shell.update(input, dt);
        if (session.wants_exit())
        {
            // Shut down first: flush progress, stop the torrent engine and
            // its threads, and remove the UPnP router mapping. quitting
            // without this leaves the mappings behind on the router.
            session.shutdown();
            sys::quit();
        }

        // Glass Phase 1: the frame is two layers. bg holds the page content
        // (artwork, text, cards); glass() blurs it into the renderer's glass
        // texture; fg holds the frosted-glass panels, which sample that blur.
        gfx::DrawList bg, fg;
        gfx::BackdropSpec backdrop;
        shell.draw(bg, fg, backdrop);
        sx::sync_fonts(renderer, ui_fonts, fonts);
        renderer.begin();
        renderer.backdrop(backdrop);
        renderer.draw(bg);
        // The blur pass is pure waste when nothing glassy will draw (e.g.
        // video playback with the OSD hidden): skip it and keep the last
        // texture, which nobody samples while fg is empty.
        if (!fg.empty())
        {
            renderer.glass();
            env.glass_tex = renderer.glass_texture();
        }
        renderer.draw(fg);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[SX] fatal: swap failed at frame %llu", static_cast<unsigned long long>(frames));
            sys::park();
        }
        // STEP 1 (Instant Board): the console splash is hidden right after the
        // first presented frame, which is already the interactive Board.
        if (!splash_hidden)
        {
            splash_hidden = true;
            sys::log("[SX] first frame presented; splash hidden: %d", sys::hide_splash_screen() ? 1 : 0);
        }
        ++frames;
        if (frames == 1)
        {
            sys::log("[SX] first frame: %zu shapes, %zu draw calls", renderer.last_instances(), renderer.last_draw_calls());
        }

        // One line every ten seconds: how fast the frames came.
        ++window_frames;
        worst_ms = std::max(worst_ms, frame_ms);
        if (now - window_start >= 10'000'000)
        {
            const double seconds = static_cast<double>(now - window_start) / 1e6;
            sys::log("[SX] %.1f fps, worst frame %.1f ms, %zu shapes, %zu draw calls", window_frames / seconds, worst_ms,
                     renderer.last_instances(), renderer.last_draw_calls());
            dlog("[SX] %.1f fps, worst frame %.1f ms", window_frames / seconds, worst_ms);
            window_start = now;
            window_frames = 0;
            worst_ms = 0.0f;
        }
    }
}
