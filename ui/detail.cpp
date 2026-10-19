#include "detail.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace sx
{

using hui::Action;
using hui::Direction;
using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::kFullUv;
using hui::gfx::Rect;
using hui::ui::text;
namespace tween = hui::tween;

namespace
{

constexpr Rect kPanel{1040.0f, 84.0f, 820.0f, 912.0f};
constexpr float kListTop = 214.0f;
constexpr float kEpisodeH = 116.0f;
constexpr float kEpisodePitch = 128.0f;
constexpr float kStreamH = 124.0f;
constexpr float kStreamPitch = 136.0f;
constexpr float kListViewH = 770.0f;

Color resolution_colour(const std::string &r)
{
    if (r == "4K")
        return Color::rgb(0x7b5bf5);
    if (r == "1080p")
        return Color::rgb(0x1d78d8);
    if (r == "720p")
        return Color::rgb(0x1a9474);
    return Color::rgb(0x5b5870);
}

// Split "HDR10  ·  Dolby Atmos" on the middle dot, trimming spaces.
std::vector<std::string> split_tags(const std::string &tags)
{
    std::vector<std::string> out;
    const std::string dot = "\xC2\xB7";
    std::string cur;
    for (std::size_t i = 0; i < tags.size();)
    {
        if (tags.compare(i, dot.size(), dot) == 0)
        {
            i += dot.size();
            std::size_t a = cur.find_first_not_of(" \t");
            std::size_t b = cur.find_last_not_of(" \t");
            if (a != std::string::npos)
                out.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        }
        else
            cur += tags[i++];
    }
    std::size_t a = cur.find_first_not_of(" \t");
    std::size_t b = cur.find_last_not_of(" \t");
    if (a != std::string::npos)
        out.push_back(cur.substr(a, b - a + 1));
    return out;
}

// "Season 1" -> "S1", "Specials" stays "Specials".
std::string season_pill_label(const std::string &name, std::size_t i)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("special") != std::string::npos)
        return "Specials";
    return "S" + std::to_string(i + 1);
}

// Info-panel person focus: Left from the lists parks the list cursor and
// focuses the directors/cast names; Cross on a name opens Search for them.
// One Detail instance exists in the app, so file-static state is safe.
bool g_info_focus = false;
int g_person = 0;
int g_saved_stream = 0;
int g_saved_episode = 0;
std::vector<Rect> g_person_rects; // rebuilt by draw_info every frame

} // namespace

Rect Detail::episode_rect(int index, bool final_position) const
{
    const float scroll = final_position ? episode_scroll_.position.target : episode_scroll_.offset();
    return {kPanel.x + 28.0f, kListTop + static_cast<float>(index) * kEpisodePitch - scroll, kPanel.w - 56.0f,
            kEpisodeH};
}

Rect Detail::stream_rect(int index, bool final_position) const
{
    const float scroll = final_position ? stream_scroll_.position.target : stream_scroll_.offset();
    const float top = (content_ && (content_->stream_addons.size() > 1 || !content_->streams_status.empty()))
                          ? kListTop + 18.0f
                          : kListTop;
    return {kPanel.x + 28.0f, top + static_cast<float>(index) * kStreamPitch - scroll, kPanel.w - 56.0f,
            kStreamH};
}

void Detail::set_content(const DetailContent *content)
{
    content_ = content;
    episode_ = 0;
    stream_ = 0;
    g_info_focus = false;
    g_person = 0;
    streams_wanted_ = false;
    hint_serial_ = content ? content->hint_serial : 0;
    streams_serial_ = content ? content->streams_serial : 0;
    episode_scroll_.position.snap(0.0f);
    stream_scroll_.position.snap(0.0f);
    set_mode(content && content->series ? Mode::episodes : Mode::streams);
}

void Detail::set_mode(Mode mode)
{
    mode_ = mode;
    swap_.start(0.3f);
    ring_.snap(mode_ == Mode::episodes ? episode_rect(episode_, true) : stream_rect(stream_, true));
}

void Detail::enter()
{
    age_ = 0.0f;
    g_info_focus = false;
    g_person = 0;
}

void Detail::refresh()
{
    if (!content_)
        return;
    const int episodes = static_cast<int>(content_->episodes.size());
    const int streams = static_cast<int>(content_->streams.size());
    if (content_->hint_serial != hint_serial_)
    {
        hint_serial_ = content_->hint_serial;
        episode_ = std::clamp(content_->episode_hint, 0, std::max(0, episodes - 1));
        episode_scroll_.position.snap(0.0f);
    }
    episode_ = std::min(episode_, std::max(0, episodes - 1));
    stream_ = std::min(stream_, std::max(0, streams - 1));
    // The streams of an episode were asked for from outside (Continue Watching): show them.
    if (content_->streams_serial != streams_serial_)
    {
        streams_serial_ = content_->streams_serial;
        if (content_->series)
        {
            set_mode(Mode::streams);
            streams_wanted_ = true;
        }
    }
    // The details arrived and it is a film after all: there are no episodes to show.
    if (!content_->series && mode_ == Mode::episodes)
        set_mode(Mode::streams);
    else if (content_->series && mode_ == Mode::streams && !streams_wanted_)
        set_mode(Mode::episodes);
}

void Detail::update(const hui::InputFrame &input, float dt)
{
    age_ += dt;
    clock_ += dt;
    if (!content_)
        return;
    // Info-panel person focus: Left from the lists parks the list cursor and
    // focuses the directors/cast names; Cross on a name opens Search for that
    // person. While focused, stream_/episode_ are parked at -1 so the shell's
    // confirm handler (which runs before update) hits the bounds checks in
    // detail_play/detail_pick_episode and can't fire underneath us.
    const int npersons =
        static_cast<int>(content_->director_names.size() + content_->cast_names.size());
    if (!g_info_focus && npersons > 0 && input.nav == Direction::left)
    {
        g_info_focus = true;
        g_person = 0;
        g_saved_stream = stream_;
        g_saved_episode = episode_;
        stream_ = -1;
        episode_ = -1;
    }
    else if (g_info_focus && input.nav == Direction::right)
    {
        g_info_focus = false;
        stream_ = g_saved_stream;
        episode_ = g_saved_episode;
    }
    if (g_info_focus)
    {
        if (npersons == 0)
        {
            g_info_focus = false;
            stream_ = g_saved_stream;
            episode_ = g_saved_episode;
        }
        else
        {
            g_person = std::clamp(g_person, 0, npersons - 1);
            if (input.nav == Direction::up)
                g_person = std::max(0, g_person - 1);
            else if (input.nav == Direction::down)
                g_person = std::min(npersons - 1, g_person + 1);
            if (input.is_pressed(Action::back))
            {
                g_info_focus = false;
                stream_ = g_saved_stream;
                episode_ = g_saved_episode;
                return;
            }
            if (input.is_pressed(Action::confirm))
            {
                const auto &d = content_->director_names;
                const std::size_t gi = static_cast<std::size_t>(g_person);
                const std::string name =
                    g_person < static_cast<int>(d.size()) ? d[gi] : content_->cast_names[gi - d.size()];
                g_info_focus = false;
                stream_ = g_saved_stream;
                episode_ = g_saved_episode;
                if (content_->on_person_search)
                    content_->on_person_search(name);
            }
            swap_.update(dt);
            return;
        }
    }
    if (mode_ == Mode::episodes)
    {
        const int count = static_cast<int>(content_->episodes.size());
        if (input.nav == Direction::up)
            episode_ = std::max(0, episode_ - 1);
        else if (input.nav == Direction::down)
            episode_ = std::min(std::max(0, count - 1), episode_ + 1);
        if (input.is_pressed(Action::confirm) && count > 0)
        {
            set_mode(Mode::streams);
            streams_wanted_ = true;
        }
    }
    else
    {
        const int count = static_cast<int>(content_->streams.size());
        if (input.nav == Direction::up)
            stream_ = std::max(0, stream_ - 1);
        else if (input.nav == Direction::down)
            stream_ = std::min(std::max(0, count - 1), stream_ + 1);
        if (input.is_pressed(Action::back) && content_->series)
        {
            set_mode(Mode::episodes);
            streams_wanted_ = false;
        }
    }
    const float e = static_cast<float>(episode_) * kEpisodePitch;
    episode_scroll_.reveal(e, e + kEpisodeH, kListViewH, 12.0f);
    const float s = static_cast<float>(stream_) * kStreamPitch;
    stream_scroll_.reveal(s, s + kStreamH, kListViewH, 12.0f);
    episode_scroll_.update(dt, 18.0f);
    stream_scroll_.update(dt, 18.0f);
    ring_.target(mode_ == Mode::episodes ? episode_rect(episode_, true) : stream_rect(stream_, true));
    ring_.update(dt, 22.0f);
    swap_.update(dt);
}

void Detail::draw_info(hui::gfx::DrawList &list) const
{
    const Title &t = content_->title;
    const float x = 110.0f;
    const float w = 840.0f;
    const float in = tween::stagger(age_, 0, 0.0f, 0.6f);
    list.push_opacity(in);
    const hui::ui::FontRef &display = env_.fonts.display;
    // Reference the cached wrap directly: no per-frame copy. The ellipsis
    // clip mutates, and cached_fit can evict the wrap entry, so that rewrite
    // goes through a small local copy, never the cached vector.
    const std::vector<std::string> &wrapped = cached_wrap(t.text_cache_, display.font, t.name, 66.0f, w);
    std::vector<std::string> clipped;
    const std::vector<std::string> *plines = &wrapped;
    if (wrapped.size() > 2)
    {
        clipped.assign(wrapped.begin(), wrapped.begin() + 2);
        clipped[1] = cached_fit(t.text_cache_, display.font, clipped[1] + " \xE2\x80\xA6", 66.0f, w);
        plines = &clipped;
    }
    const std::vector<std::string> &lines = *plines;
    float baseline = 206.0f;
    // The title: the show's own logo art when it has arrived, drawn at its own
    // aspect and never stretched; otherwise the name as text. The meta row
    // below anchors to the bottom of whichever title was drawn, so a short
    // logo leaves no gap.
    if (t.logo != 0 && t.logo_w > 0 && t.logo_h > 0)
    {
        float lw = std::min(420.0f, static_cast<float>(t.logo_w));
        float lh = lw * static_cast<float>(t.logo_h) / static_cast<float>(t.logo_w);
        if (lh > 130.0f) // cap tall logos so the metadata, synopsis, and cast always fit below
        {
            lh = 130.0f;
            lw = lh * static_cast<float>(t.logo_w) / static_cast<float>(t.logo_h);
        }
        list.image(t.logo, {x, 148.0f, lw, lh}, kFullUv, Color::rgb(0xffffff));
        // Add +44px below the logo's bottom edge so 26px text has 18px of clean air above its cap-height.
        baseline = 148.0f + lh + 44.0f;
    }
    else
    {
        for (const std::string &line : lines)
        {
            text(list, display, cached_fit(t.text_cache_, display.font, line, 66.0f, w), x, baseline, 66.0f,
                 theme::ink, Align::left, -1.4f);
            baseline += 74.0f;
        }
        baseline += 2.0f;
    }
    std::string meta = t.year;
    if (!t.runtime.empty())
        meta += "   \xC2\xB7   " + t.runtime;
    if (!t.genres.empty())
        meta += "   \xC2\xB7   " + t.genres;
    // The rating badge follows the line, so the line leaves room for it before the panel.
    const std::string &fitted_meta =
        cached_fit(text_cache_, env_.fonts.regular.font, meta, 26.0f, t.imdb.empty() ? w : w - 190.0f);
    const float meta_w = text(list, env_.fonts.regular, fitted_meta, x, baseline, 26.0f, theme::ink_2);
    draw_imdb(list, env_, t.imdb, x + meta_w + 30.0f, baseline);
    baseline += 58.0f;
    // Episodes carry their own synopsis: show the focused episode's when it has
    // one, else the series synopsis as before.
    std::string synopsis = t.synopsis;
    if (content_->series && episode_ >= 0 &&
        static_cast<std::size_t>(episode_) < content_->episodes.size() &&
        !content_->episodes[static_cast<std::size_t>(episode_)].overview.empty())
        synopsis = content_->episodes[static_cast<std::size_t>(episode_)].overview;
    baseline = cached_paragraph(list, text_cache_, env_.fonts.regular, synopsis, x, baseline, 27.0f, w - 40.0f,
                                41.0f, theme::ink_2, 6);
    baseline += 18.0f;
    if (!content_->writers.empty())
    {
        text(list, env_.fonts.regular, "Writers", x, baseline, 22.0f, theme::ink_3);
        text(list, env_.fonts.regular,
             cached_fit(text_cache_, env_.fonts.regular.font, content_->writers, 24.0f, w - 190.0f), x + 150.0f,
             baseline, 24.0f, theme::ink_2);
        baseline += 42.0f;
    }
    // People: every director/cast name is its own row. Left from the lists
    // focuses these rows; the focused name gets an accent pill and Cross
    // opens Search for that person.
    g_person_rects.clear();
    const auto &dnames = content_->director_names;
    const auto &cnames = content_->cast_names;
    int prow = 0;
    const auto person_row = [&](const std::string &label, const std::string &name, bool show_label) {
        const float nx = x + 150.0f;
        if (show_label)
            text(list, env_.fonts.regular, label, x, baseline, 22.0f, theme::ink_3);
        const bool focused = g_info_focus && prow == g_person;
        const float nw = env_.fonts.regular.measure(name, 24.0f);
        const Rect hr{nx - 12.0f, baseline - 30.0f, nw + 24.0f, 38.0f};
        if (focused)
            list.rounded_rect(hr, 19.0f, theme::accent.with_alpha(0.38f));
        text(list, env_.fonts.regular, name, nx, baseline, 24.0f, focused ? theme::ink : theme::ink_2);
        g_person_rects.push_back(hr);
        baseline += 42.0f;
        ++prow;
    };
    for (std::size_t i = 0; i < dnames.size(); ++i)
        person_row(content_->series ? "Creators" : "Director", dnames[i], i == 0);
    for (std::size_t i = 0; i < cnames.size(); ++i)
        person_row("Cast", cnames[i], i == 0);
    list.pop_opacity();
}

void Detail::draw_episodes(hui::gfx::DrawList &fg) const
{
    // The season bar: L1 and R1 turn the pages.
    const Rect bar{kPanel.x + 28.0f, 112.0f, kPanel.w - 56.0f, 76.0f};
    draw_glass_panel(fg, env_, bar, 26.0f, theme::glass_fill);
    const std::size_t nseasons = content_->seasons.size();
    if (nseasons > 1 && nseasons <= 8)
    {
        // Compact pill row: "S1", "S2", ... or "Specials", accent on the active one.
        std::vector<std::string> labels;
        std::vector<float> widths;
        float total = 0.0f;
        for (std::size_t i = 0; i < nseasons; ++i)
        {
            std::string lbl = season_pill_label(content_->seasons[i], i);
            if (i == static_cast<std::size_t>(content_->season))
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s  ·  %zu ep", lbl.c_str(), content_->episodes.size());
                lbl = buf;
            }
            labels.push_back(lbl);
            const float w = env_.fonts.semibold.measure(lbl, 20.0f) + 30.0f;
            widths.push_back(w);
            total += w + 10.0f;
        }
        total -= 10.0f;
        float px = bar.cx() - total * 0.5f;
        for (std::size_t i = 0; i < nseasons; ++i)
        {
            const bool active = i == static_cast<std::size_t>(content_->season);
            fg.rounded_rect({px, bar.cy() - 17.0f, widths[i], 34.0f}, 17.0f,
                            active ? theme::accent : Color::rgb(0xffffff, 0.08f));
            text(fg, env_.fonts.semibold, labels[i], px + widths[i] * 0.5f, bar.cy() + 7.0f, 20.0f,
                 active ? Color::rgb(0xffffff) : theme::ink_2, Align::center);
            px += widths[i] + 10.0f;
        }
    }
    else
    {
        const std::string season = cached_fit(text_cache_, env_.fonts.semibold.font,
            content_->seasons.empty() ? "Episodes" : content_->seasons[std::min<std::size_t>(content_->season, content_->seasons.size() - 1)],
            30.0f, bar.w - 220.0f);
        char count[48];
        std::snprintf(count, sizeof(count), "%zu episodes", content_->episodes.size());
        const float sw = env_.fonts.semibold.measure(season, 30.0f);
        const float cw = env_.fonts.regular.measure(count, 24.0f);
        const float start = bar.cx() - (sw + 22.0f + cw) * 0.5f;
        text(fg, env_.fonts.semibold, season, start, bar.cy() + 11.0f, 30.0f, theme::ink);
        text(fg, env_.fonts.regular, count, start + sw + 22.0f, bar.cy() + 8.0f, 24.0f, theme::ink_3);
    }
    hui::ui::draw_button(fg, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::l1, bar.x + 22.0f, bar.cy(),
                         40.0f);
    hui::ui::draw_button(fg, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::r1, bar.x + bar.w - 66.0f,
                         bar.cy(), 40.0f);

    fg.push_clip({kPanel.x, kListTop - 8.0f, kPanel.w, kListViewH + 16.0f});
    const Rect ring = ring_.value();
    if (!content_->episodes.empty() && !g_info_focus)
        fg.glow(ring, 24.0f, 24.0f, theme::accent.with_alpha(0.30f));
    for (int i = 0; i < static_cast<int>(content_->episodes.size()); ++i)
    {
        const Rect r = episode_rect(i, false);
        if (r.y > 1080.0f || r.y + r.h < 0.0f)
            continue;
        const Episode &e = content_->episodes[i];
        const bool focused = i == episode_;
        const float enter = tween::stagger(age_, i, 0.04f, 0.4f);
        fg.push_opacity(enter);
        if (focused)
            draw_glass_panel(fg, env_, r, 24.0f, theme::glass_fill);
        else
            fg.rounded_rect(r, 24.0f, Color::rgb(0xffffff, 0.045f));
        const Rect thumb{r.x + 12.0f, r.y + 11.0f, 168.0f, 94.0f};
        if (e.thumb != 0)
            fg.image(e.thumb, thumb, kFullUv, Color::rgb(0xffffff), 12.0f);
        else
            fg.gradient_rect(thumb, 12.0f, Color::rgb(0x2a2540), Color::rgb(0x14111f));
        if (e.progress >= 0.0f)
        {
            fg.rounded_rect({thumb.x + 8.0f, thumb.y + thumb.h - 14.0f, thumb.w - 16.0f, 5.0f}, 2.5f,
                              Color::rgb(0xffffff, 0.30f));
            fg.rounded_rect({thumb.x + 8.0f, thumb.y + thumb.h - 14.0f, (thumb.w - 16.0f) * e.progress, 5.0f}, 2.5f,
                              theme::accent_soft);
        }
        const float tx = thumb.x + thumb.w + 22.0f;
        const float room = r.x + r.w - tx - 74.0f;
        char number[16];
        std::snprintf(number, sizeof(number), "%d", e.number);
        const float nw = text(fg, env_.fonts.semibold, number, tx, r.y + 47.0f, 26.0f, theme::ink_3) + 14.0f;
        text(fg, env_.fonts.semibold, cached_fit(e.text_cache_, env_.fonts.semibold.font, e.title, 26.0f, room - nw),
             tx + nw, r.y + 47.0f, 26.0f, e.watched ? theme::ink_3 : theme::ink);
        text(fg, env_.fonts.regular, e.info, tx, r.y + 83.0f, 22.0f, theme::ink_3);
        if (e.watched)
        {
            const float cx = r.x + r.w - 40.0f, cy = r.cy();
            fg.circle(cx, cy, 17.0f, theme::watched);
            fg.line(cx - 7.0f, cy + 1.0f, cx - 2.0f, cy + 6.5f, 3.4f, Color::rgb(0xffffff));
            fg.line(cx - 2.0f, cy + 6.5f, cx + 8.0f, cy - 6.0f, 3.4f, Color::rgb(0xffffff));
        }
        fg.pop_opacity();
    }
    // Loading skeletons: while the episode list is still arriving, shimmer 5
    // placeholder rows (thumb + two text lines each) so the panel does not
    // sit empty. Same geometry as a real row via episode_rect(i, false).
    if (content_->episodes.empty() && content_->episodes_status.rfind("Loading", 0) == 0)
    {
        for (int i = 0; i < 5; ++i)
        {
            const Rect r = episode_rect(i, false);
            if (r.y > 1080.0f || r.y + r.h < 0.0f)
                continue;
            const float a = 0.06f + 0.04f * std::sin(clock_ * 2.2f - static_cast<float>(i) * 0.5f);
            fg.rounded_rect(r, 24.0f, Color::rgb(0xffffff, a));
            fg.rounded_rect({r.x + 12.0f, r.y + 11.0f, 168.0f, 94.0f}, 12.0f, Color::rgb(0xffffff, a));
            fg.rounded_rect({r.x + 202.0f, r.y + 32.0f, r.w - 300.0f, 22.0f}, 11.0f, Color::rgb(0xffffff, a));
            fg.rounded_rect({r.x + 202.0f, r.y + 66.0f, r.w - 420.0f, 18.0f}, 9.0f, Color::rgb(0xffffff, a));
        }
    }
    if (!content_->episodes.empty() && !g_info_focus)
        fg.bordered_rect(ring, 24.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
    fg.pop_clip();
    if (content_->episodes.empty())
        cached_paragraph(fg, text_cache_, env_.fonts.regular, content_->episodes_status, kPanel.x + 44.0f,
                         kListTop + 56.0f, 26.0f, kPanel.w - 88.0f, 38.0f, theme::ink_3, 5);
}

void Detail::draw_streams(hui::gfx::DrawList &fg) const
{
    // The heading, with the way back when this follows an episode.
    const float hx = kPanel.x + 40.0f;
    float tx = hx;
    if (content_->series)
    {
        draw_chevron(fg, hx + 8.0f, 149.0f, 20.0f, 2, theme::ink_2);
        tx = hx + 38.0f;
    }
    char count[48];
    std::snprintf(count, sizeof(count), "%zu streams", content_->streams.size());
    const float cw = env_.fonts.regular.measure(count, 24.0f);
    text(fg, env_.fonts.regular, count, kPanel.x + kPanel.w - 40.0f, 160.0f, 24.0f, theme::ink_3, Align::right);
    const std::string heading = content_->stream_heading.empty() ? "Streams" : content_->stream_heading;
    text(fg, env_.fonts.semibold, cached_fit(text_cache_, env_.fonts.semibold.font, heading, 32.0f, kPanel.w - 120.0f - cw),
         tx, 160.0f, 32.0f, theme::ink);
    // Addon source tabs: filter streams by addon (L1/R1). Only when >= 2 addons.
    if (content_->stream_addons.size() > 1)
    {
        float tab_x = hx;
        const float tab_y = 176.0f;
        for (std::size_t i = 0; i < content_->stream_addons.size(); ++i)
        {
            const bool sel = static_cast<int>(i) == content_->stream_addon;
            const float fs = 20.0f;
            float w = env_.fonts.semibold.measure(content_->stream_addons[i], fs) + 28.0f;
            if (w > 200.0f)
                w = 200.0f;
            if (tab_x + w > kPanel.x + kPanel.w - 116.0f)
                break;
            if (sel)
                fg.rounded_rect({tab_x, tab_y, w, 30.0f}, 15.0f, theme::accent.with_alpha(0.25f));
            const std::string &label =
                cached_fit(text_cache_, env_.fonts.semibold.font, content_->stream_addons[i], fs, w - 28.0f);
            text(fg, env_.fonts.semibold, label, tab_x + w * 0.5f, tab_y + 22.0f, fs,
                 sel ? theme::ink : theme::ink_3, Align::center);
            tab_x += w + 6.0f;
        }
        // L1/R1 hint at the right edge.
        text(fg, env_.fonts.regular, "L1 / R1", kPanel.x + kPanel.w - 40.0f, tab_y + 21.0f,
             18.0f, theme::ink_3, Align::right);
    }
    // While addons are still answering, the list below keeps growing: show the
    // "Loading streams... (x of y)" status under the heading, not only when the
    // list is empty, so nobody presses Play on a partial list unaware.
    // (Skipped when addon tabs are visible to avoid overlap.)
    if (!content_->streams.empty() && !content_->streams_status.empty() && content_->stream_addons.size() <= 1)
        text(fg, env_.fonts.regular,
             cached_fit(text_cache_, env_.fonts.regular.font, content_->streams_status, 22.0f, kPanel.w - 88.0f), hx,
             194.0f, 22.0f, theme::ink_3);

    const float top = (content_ && (content_->stream_addons.size() > 1 || !content_->streams_status.empty()))
                          ? kListTop + 18.0f
                          : kListTop;
    fg.push_clip({kPanel.x, top - 8.0f, kPanel.w, kListViewH - (top - kListTop) + 16.0f});
    const Rect ring = ring_.value();
    if (!content_->streams.empty() && !g_info_focus)
        fg.glow(ring, 24.0f, 24.0f, theme::accent.with_alpha(0.30f));
    for (int i = 0; i < static_cast<int>(content_->streams.size()); ++i)
    {
        const Rect r = stream_rect(i, false);
        if (r.y > 1080.0f || r.y + r.h < 0.0f)
            continue;
        const Stream &s = content_->streams[i];
        const bool focused = i == stream_;
        const float enter = tween::stagger(age_, i, 0.04f, 0.4f);
        fg.push_opacity(enter);
        if (focused)
            draw_glass_panel(fg, env_, r, 24.0f, theme::glass_fill);
        else
            fg.rounded_rect(r, 24.0f, Color::rgb(0xffffff, 0.045f));
        // The resolution, as a tinted glass pill in its own colour.
        const Color rc = resolution_colour(s.resolution);
        const Rect badge{r.x + 20.0f, r.cy() - 28.0f, 104.0f, 56.0f};
        fg.bordered_rect(badge, 16.0f, rc.with_alpha(0.22f), 2.0f, hui::gfx::mix(rc, Color::rgb(0xffffff), 0.25f));
        text(fg, env_.fonts.semibold, s.resolution, badge.cx(), badge.cy() + 9.0f, 26.0f, Color::rgb(0xffffff),
             Align::center);
        const float x = badge.x + badge.w + 24.0f;
        const float right = r.x + r.w - 24.0f;
        const float room = right - x - 150.0f;
        text(fg, env_.fonts.semibold, cached_fit(s.text_cache_, env_.fonts.semibold.font, s.name, 26.0f, room), x,
             r.y + 35.0f, 26.0f, focused ? theme::ink : theme::ink_2);
        text(fg, env_.fonts.regular, cached_fit(s.text_cache_, env_.fonts.regular.font, s.detail, 22.0f, room), x,
             r.y + 64.0f, 22.0f, theme::ink_3);
        if (!s.tags.empty())
        {
            // Color-coded hardware/format pills instead of one plain string.
            float tx = x;
            const float tag_limit = right - 150.0f;
            for (const std::string &tag : split_tags(s.tags))
            {
                Color fill, ink;
                if (tag == "Dolby Vision" || tag == "HDR10+" || tag == "HDR10" || tag == "HDR")
                {
                    fill = Color::rgb(0xf5c518, 0.20f);
                    ink = Color::rgb(0xffdf6b);
                }
                else if (tag == "Dolby Atmos" || tag == "TrueHD" || tag == "DTS-HD" || tag == "7.1" || tag == "5.1")
                {
                    fill = theme::accent.with_alpha(0.24f);
                    ink = theme::accent_soft;
                }
                else
                {
                    fill = Color::rgb(0xffffff, 0.08f);
                    ink = theme::ink_2;
                }
                const float w = draw_tag(fg, env_, tag, tx, r.y + 96.0f, fill, ink);
                tx += w + 8.0f;
                if (tx > tag_limit)
                    break;
            }
        }
        text(fg, env_.fonts.semibold, s.size, right, r.y + 48.0f, 26.0f, theme::ink, Align::right);
        if (!s.seeds.empty())
        {
            const int n = std::atoi(s.seeds.c_str());
            const Color seed_c = n >= 20 ? Color::rgb(0x22b365)
                               : n >= 1  ? Color::rgb(0xf5a623)
                                         : Color::rgb(0xe5566a);
            const float seedw = env_.fonts.regular.measure(s.seeds, 22.0f);
            fg.circle(right - seedw - 18.0f, r.y + 74.0f, 6.0f, seed_c);
            text(fg, env_.fonts.regular, s.seeds, right, r.y + 82.0f, 22.0f, seed_c, Align::right);
        }
        fg.pop_opacity();
    }
    if (!content_->streams.empty() && !g_info_focus)
        fg.bordered_rect(ring, 24.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
    fg.pop_clip();
    if (content_->streams.empty())
        cached_paragraph(fg, text_cache_, env_.fonts.regular, content_->streams_status, kPanel.x + 44.0f,
                         kListTop + 56.0f, 26.0f, kPanel.w - 88.0f, 38.0f, theme::ink_3, 5);
}

void Detail::draw_hints_row(hui::gfx::DrawList &list) const
{
    hui::ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 24.0f;
    layout.cy = 1038.0f;
    layout.item_gap = 40.0f;
    layout.font = &env_.fonts.regular;
    if (mode_ == Mode::episodes)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Streams"},
                                       {hui::ui::Button::square, "Choose stream"},
                                       {hui::ui::Button::l1, "Season", hui::ui::Button::r1},
                                       {hui::ui::Button::options, "More"},
                                       {hui::ui::Button::circle, "Back"}};
        hui::ui::draw_hints(list, env_.fonts, hui::ui::GlyphStyle::dark(), hints, 5, theme::kRight, true, layout);
    }
    else
    {
        if (content_ && content_->stream_addons.size() > 1)
        {
            const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Play"},
                                           {hui::ui::Button::square, "Play with server"},
                                           {hui::ui::Button::l1, "Addon", hui::ui::Button::r1},
                                           {hui::ui::Button::options, "More"},
                                           {hui::ui::Button::circle, "Back"}};
            hui::ui::draw_hints(list, env_.fonts, hui::ui::GlyphStyle::dark(), hints, 5, theme::kRight, true, layout);
        }
        else
        {
            const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Play"},
                                           {hui::ui::Button::square, "Play with server"},
                                           {hui::ui::Button::options, "More"},
                                           {hui::ui::Button::circle, "Back"}};
            hui::ui::draw_hints(list, env_.fonts, hui::ui::GlyphStyle::dark(), hints, 4, theme::kRight, true, layout);
        }
    }
}

void Detail::draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, hui::gfx::BackdropSpec &backdrop) const
{
    backdrop.mode = hui::gfx::BackdropMode::none;
    bg.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, theme::ground);
    if (!content_)
        return;
    const float intro = tween::stagger(age_, 0, 0.0f, 0.8f);
    // The artwork fills the screen, then the page closes over it where text goes.
    if (content_->title.backdrop != 0)
    {
        bg.push_opacity(intro);
        bg.image(content_->title.backdrop, {0, 0, theme::kScreenW, theme::kScreenH}, kFullUv,
                   Color::rgb(0xffffff));
        // Same darkness as the home hero: a uniform 60% black over the full screen.
        bg.gradient_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, theme::ground.with_alpha(0.60f),
                         theme::ground.with_alpha(0.60f));
        bg.pop_opacity();
    }
    bg.gradient_rect_h({0, 0, 1400.0f, theme::kScreenH}, 0, theme::ground.with_alpha(0.93f),
                         theme::ground.with_alpha(0.16f));
    bg.gradient_rect({0, 640.0f, theme::kScreenW, 440.0f}, 0, theme::ground.with_alpha(0.0f),
                       theme::ground.with_alpha(0.88f));
    bg.gradient_rect({0, 0, theme::kScreenW, 200.0f}, 0, theme::ground.with_alpha(0.6f),
                       theme::ground.with_alpha(0.0f));
    // Ambilight: a subtle accent-tinted wash at the top-right, blended 30%
    // with the ground colour so it stays dark-theme safe.
    {
        const std::uint32_t ac = content_->title.accent;
        const Color wash = hui::gfx::mix(
            theme::ground,
            Color{static_cast<float>((ac >> 16) & 0xff) / 255.0f,
                  static_cast<float>((ac >> 8) & 0xff) / 255.0f,
                  static_cast<float>(ac & 0xff) / 255.0f, 1.0f},
            0.30f);
        bg.gradient_rect_h({theme::kScreenW - 1100.0f, 0, 1100.0f, 380.0f}, 0,
                           wash.with_alpha(0.0f), wash.with_alpha(0.45f));
    }

    bg.push_opacity(intro);
    draw_logo(bg, env_);
    bg.pop_opacity();
    draw_info(bg);

    // The panel.
    const float panel_in = tween::stagger(age_, 1, 0.08f, 0.5f);
    fg.push_opacity(panel_in);
    fg.shadow({kPanel.x + 8, kPanel.y + 26, kPanel.w - 16, kPanel.h - 20}, 34, 56, Color::rgb(0x000000, 0.45f));
    draw_glass_panel(fg, env_, kPanel, 36.0f, Color::rgb(0x101012, 0.86f));
    const float swap = tween::smoothstep(swap_.progress());
    fg.push_opacity(swap);
    if (mode_ == Mode::episodes)
        draw_episodes(fg);
    else
        draw_streams(fg);
    fg.pop_opacity();
    fg.pop_opacity();
    draw_hints_row(bg);
}

} // namespace sx
