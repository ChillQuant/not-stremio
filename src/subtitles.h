#pragma once

#include <string>
#include <vector>

struct Cue {
	double start = 0, end = 0;
	std::string rml;  // escaped, with <em>/<strong>, <span style="color:..."> and <br/>
	int an = 2;       // ASS \an alignment (numpad 1-9), 2 = bottom-center
};

// Parses SubRip, WebVTT, SubStation Alpha and MicroDVD text (UTF-8 already).
// Returns cues sorted by start time.
std::vector<Cue> parse_subtitle_file(const std::string& text);

// Turns one cue's text (SRT/VTT-style markup or ASS override tags, "\N"
// line breaks) into RML. When `an` is given, an ASS \an alignment sets it
// (left at 2 otherwise).
std::string subtitle_text_to_rml(const std::string& text, int* an = nullptr);

// Text of the cues active at `t` joined with line breaks, "" when none.
// When `an` is given, the most recent active cue's alignment sets it.
std::string cues_at(const std::vector<Cue>& cues, double t, int* an = nullptr);

// ---------------------------------------------------------------------------
// Subtitle behavior + lookback (UI thread only; plain data, no locks).
// ---------------------------------------------------------------------------

// How ASS styling is resolved at subtitle load. Replaces the old binary
// on/off; style resolution happens once at load, the per-frame path
// (cues_at / draw) is unchanged.
enum class AssMode
{
    authored, // subtitle file styles untouched (colors, italics, karaoke, \an)
    readable, // default: keeps positioning/colors/karaoke, enforces the
              // readability floor (minimum outline, never a transparent one)
    plain     // strips colors/italics/bold; the user's size/color always win
    // (positioning via \an is kept in every mode so signs never merge into
    // dialogue)
};

class Subtitles
{
  public:
    static void set_ass_mode(AssMode m);
    static AssMode ass_mode();

    // Lookback ("what did they just say?"): the last lines actually drawn.
    // note_shown is fed from the subtitle draw path on each cue change;
    // recent_lines returns the last n in display order, oldest first.
    static void note_shown(const std::string& plain_line);
    static std::vector<std::string> recent_lines(int n);

    // One-press sync readout: nudge_sub_delay (session_watch.cpp) should call
    // note_delay_ms so the subtitle overlay can show a transient readout
    // next to the subtitles ("Subtitle delay +0.25 s"). The nudge itself and
    // its toast stay in session_watch.cpp; this is only the overlay hook.
    static void note_delay_ms(int ms);
    static int delay_ms();           // last noted value, 0 until noted
    static unsigned delay_serial();  // increments on every note_delay_ms

  private:
    static AssMode mode_;
    static std::vector<std::string> history_;
    static int delay_ms_;
    static unsigned delay_serial_;
};

// True when a track label carries SDH / hearing-impaired markers. Never
// auto-select such a track; the user asks for it explicitly.
bool sub_track_is_sdh(const std::string& label);

// "English (ASS, SDH)" / "Thai (SRT)" / "Japanese (Signs only)":
// turns raw addon/embedded/USB track data into a human track label.
// lang_iso3 is ISO-639-2 ("eng"); url_hint is the file URL or filename
// (used for the format tag). When the language is unknown the raw label
// (e.g. the filename stem) is kept as the base instead of "Unknown".
std::string format_sub_track_label(const std::string& lang_iso3, const std::string& raw_label,
                                   const std::string& url_hint);
