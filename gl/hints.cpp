#include "hints.hpp"

#include "chrome.hpp"
#include "theme.hpp"
#include "ui/fonts.hpp"

#include <algorithm>
#include <cstring>

#if __has_include("a11y.hpp")
#include "a11y.hpp"
#endif

namespace sx
{
namespace Hints
{
namespace
{

constexpr double kFadeAfter = 5.0; // seconds of no input before the bar fades
constexpr float kFadeTime = 0.45f; // fade in/out duration

std::string text_;
float alpha_ = 0.0f;
float alpha_target_ = 0.0f;
Context context_ = Context::grid;
double last_idle_ = 0.0;
bool has_last_idle_ = false;

bool reduce_motion()
{
#if __has_include("a11y.hpp")
    return A11y::get().reduce_motion;
#else
    return false;
#endif
}

float text_scale()
{
#if __has_include("a11y.hpp")
    const float s = A11y::get().ui_text_scale;
    return s > 0.0f ? s : 1.0f;
#else
    return 1.0f;
#endif
}

const char *default_text(Context c)
{
    switch (c)
    {
    case Context::playback:
        return "R3 Stats \xc2\xb7 L3 Restart \xc2\xb7 Options More";
    case Context::detail:
        return "Options: Mark watched \xc2\xb7 Circle: Back";
    case Context::search:
        return "Triangle: New search \xc2\xb7 Circle: Back";
    case Context::grid:
    default:
        return "Options: Mark watched";
    }
}

} // namespace

void update(float dt, Context context, const char *context_text, double idle_seconds)
{
    bool activity = false;
    if (context != context_)
    {
        context_ = context;
        const char *t = (context_text && context_text[0]) ? context_text : default_text(context);
        text_ = t ? t : "";
        activity = true; // a new screen introduces its hints
    }
    // The idle clock was reset since the last frame: a button was pressed.
    if (has_last_idle_ && idle_seconds < last_idle_ - 1e-6)
        activity = true;
    has_last_idle_ = true;
    last_idle_ = idle_seconds;

    if (activity)
        alpha_target_ = 1.0f;
    else if (idle_seconds >= kFadeAfter)
        alpha_target_ = 0.0f;

    if (reduce_motion())
        alpha_ = alpha_target_; // no animation: instant cut
    else
    {
        const float step = dt / kFadeTime;
        if (alpha_ < alpha_target_)
            alpha_ = std::min(alpha_target_, alpha_ + step);
        else
            alpha_ = std::max(alpha_target_, alpha_ - step);
    }
}

void draw(hui::gfx::DrawList &list, const Env &env)
{
    if (alpha_ < 0.01f || text_.empty())
        return;
    constexpr float kSize = 24.0f;
    const float size = kSize * text_scale();
    const hui::ui::FontRef &font = env.fonts.regular;
    if (!font.font)
        return; // fonts not loaded yet: nothing to measure with
    const float text_w = font.measure(text_, size);
    const float w = text_w + 56.0f;
    const float h = 52.0f;
    const float x = (theme::kScreenW - w) * 0.5f;
    // During playback the bottom holds the subtitles (~960) and the controls
    // row (978-1042): go to the top like the toast, but below its zone
    // (140..224) so the two never stack. Otherwise bottom-center.
    const float y = context_ == Context::playback ? 236.0f : theme::kScreenH - 96.0f;
    list.push_opacity(alpha_);
    // Neutral-black dim pill, never violet.
    list.rounded_rect({x, y, w, h}, 26.0f, hui::gfx::Color::rgb(0x000000, 0.55f));
    hui::ui::text(list, font, text_, x + 28.0f, y + h * 0.5f + size * 0.36f, size,
                  hui::gfx::Color::rgb(0xffffff, 0.75f), hui::gfx::Align::left);
    list.pop_opacity();
}

const std::string &text()
{
    return text_;
}

float alpha()
{
    return alpha_;
}

} // namespace Hints
} // namespace sx
