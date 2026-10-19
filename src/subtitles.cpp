#include "subtitles.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "bidi.h"
#include "stremio.h"
#include "util.h"

// Global subtitle behavior (UI thread only).
AssMode Subtitles::mode_ = AssMode::readable;
std::vector<std::string> Subtitles::history_;
int Subtitles::delay_ms_ = 0;
unsigned Subtitles::delay_serial_ = 0;

void Subtitles::set_ass_mode(AssMode m) { mode_ = m; }
AssMode Subtitles::ass_mode() { return mode_; }

void Subtitles::note_shown(const std::string& plain_line) {
	if (plain_line.empty()) return;
	// Consecutive duplicates (e.g. a size change re-wrapping the same cue)
	// don't add history entries.
	if (!history_.empty() && history_.back() == plain_line) return;
	history_.push_back(plain_line);
	if (history_.size() > 50) history_.erase(history_.begin());
}

std::vector<std::string> Subtitles::recent_lines(int n) {
	if (n <= 0) return {};
	const std::size_t k = std::min<std::size_t>(static_cast<std::size_t>(n), history_.size());
	return std::vector<std::string>(history_.end() - k, history_.end());
}

void Subtitles::note_delay_ms(int ms) {
	delay_ms_ = ms;
	++delay_serial_;
}
int Subtitles::delay_ms() { return delay_ms_; }
unsigned Subtitles::delay_serial() { return delay_serial_; }

// "00:01:02,345" / "01:02.345" / "0:01:02.34" -> seconds; -1 if not a time.
static double parse_timestamp(const std::string& s) {
	std::vector<double> parts;
	std::string cur;
	for (char c : s) {
		if (c == ':') {
			if (cur.empty()) return -1;
			parts.push_back(atof(cur.c_str()));
			cur.clear();
		} else if (isdigit((unsigned char)c) || c == '.' || c == ',') {
			cur += (c == ',') ? '.' : c;
		} else {
			return -1;
		}
	}
	if (cur.empty()) return -1;
	parts.push_back(atof(cur.c_str()));
	if (parts.size() < 2 || parts.size() > 3) return -1;
	double t = 0;
	for (double p : parts) t = t * 60 + p;
	return t;
}

static int hex_nybble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Finds `key` (\c or \1c) followed by &HBBGGRR& in an override block and
// returns "#RRGGBB"; "" when absent or malformed. The key must sit directly
// after the backslash, so \clip and friends can never match.
static std::string ass_color(const std::string& tags, const char* key) {
	const size_t klen = std::char_traits<char>::length(key);
	size_t p = tags.find(key);
	while (p != std::string::npos) {
		if (p + klen + 2 < tags.size() && tags.compare(p + klen, 2, "&H") == 0) {
			size_t e = tags.find('&', p + klen + 2);
			std::string hex = e == std::string::npos ? "" : tags.substr(p + klen + 2, e - p - klen - 2);
			if (hex.size() >= 6) {
				hex = hex.substr(hex.size() - 6); // &HAABBGGRR form: alpha first
				int b0 = hex_nybble(hex[0]), b1 = hex_nybble(hex[1]);
				int g0 = hex_nybble(hex[2]), g1 = hex_nybble(hex[3]);
				int r0 = hex_nybble(hex[4]), r1 = hex_nybble(hex[5]);
				if (b0 >= 0 && b1 >= 0 && g0 >= 0 && g1 >= 0 && r0 >= 0 && r1 >= 0) {
					const char* digits = "0123456789ABCDEF";
					std::string out = "#";
					out += digits[r0];
					out += digits[r1];
					out += digits[g0];
					out += digits[g1];
					out += digits[b0];
					out += digits[b1];
					return out;
				}
			}
		}
		p = tags.find(key, p + 1);
	}
	return "";
}

std::string subtitle_text_to_rml(const std::string& in, int* an) {
	std::string out;
	bool italic = false, bold = false, span_open = false, drawing = false;
	// Plain mode strips colors/italics/bold; the user's size/color always win.
	// (Positioning via \an is kept in every mode so signs never merge into
	// dialogue.)
	const bool plain = Subtitles::ass_mode() == AssMode::plain;
	size_t i = 0;
	while (i < in.size()) {
		char c = in[i];
		if (c == '{' && i + 1 < in.size() && in[i + 1] == '\\') {
			// ASS override block: honour italics/bold/color/alignment;
			// karaoke timing (\k) is ignored (the text stays), \pos has no
			// positioned-sign support in the OSD and is dropped with the rest.
			size_t end = in.find('}', i);
			if (end == std::string::npos) break;
			std::string tags = in.substr(i, end - i);
			// {\p1}..{\p9} enters ASS vector polygon drawing mode (background
			// signs); {\p0} or {\r} exits it. While drawing, the raw vector
			// coordinates are skipped so they don't print as gibberish.
			size_t pp = tags.find("\\p");
			while (pp != std::string::npos) {
				if (pp + 2 < tags.size() && tags[pp + 2] >= '0' && tags[pp + 2] <= '9') {
					drawing = (tags[pp + 2] != '0');
				}
				pp = tags.find("\\p", pp + 2);
			}
			if (tags.find("\\r") != std::string::npos)
				drawing = false;
			// {\an1}..{\an9}: numpad alignment for this cue (kept in every
			// mode, including Plain, so signs never merge into dialogue).
			size_t ap = tags.find("\\an");
			if (ap != std::string::npos && ap + 3 < tags.size() && tags[ap + 3] >= '1' &&
			    tags[ap + 3] <= '9' && an)
				*an = tags[ap + 3] - '0';
			if (plain) {
				i = end + 1;
				continue;
			}
			if (tags.find("\\i1") != std::string::npos && !italic) out += "<em>", italic = true;
			if (tags.find("\\i0") != std::string::npos && italic) out += "</em>", italic = false;
			if (tags.find("\\b1") != std::string::npos && !bold) out += "<strong>", bold = true;
			if (tags.find("\\b0") != std::string::npos && bold) out += "</strong>", bold = false;
			// {\c&HBBGGRR&} / {\1c&HBBGGRR&}: primary text color (ASS hex is
			// blue-green-red, with an optional alpha pair first).
			std::string color = ass_color(tags, "\\1c");
			if (color.empty()) color = ass_color(tags, "\\c");
			if (!color.empty()) {
				if (span_open) out += "</span>";
				out += "<span style=\"color:" + color + "\">";
				span_open = true;
			}
			i = end + 1;
			continue;
		}
		if (c == '{' && i + 1 < in.size() && in[i + 1] == 'y' && in.find('}', i) != std::string::npos) {
			// MicroDVD {y:i}
			size_t end = in.find('}', i);
			if (in.compare(i, 4, "{y:i") == 0 && !italic && !plain) out += "<em>", italic = true;
			i = end + 1;
			continue;
		}
		if (c == '\\' && i + 1 < in.size() && (in[i + 1] == 'N' || in[i + 1] == 'n')) {
			out += "<br/>";
			i += 2;
			continue;
		}
		if (c == '\\' && i + 1 < in.size() && in[i + 1] == 'h') {
			out += ' ';
			i += 2;
			continue;
		}
		if (c == '<') {
			size_t end = in.find('>', i);
			if (end != std::string::npos) {
				std::string tag = lower(in.substr(i + 1, end - i - 1));
				if (tag == "i" && !italic && !plain) out += "<em>", italic = true;
				else if (tag == "/i" && italic) out += "</em>", italic = false;
				else if (tag == "b" && !bold && !plain) out += "<strong>", bold = true;
				else if (tag == "/b" && bold) out += "</strong>", bold = false;
				else if (tag == "br" || tag == "br/" || tag == "br /") out += "<br/>";
				// <font>, <u>, <c.yellow>, <v Name>, VTT timestamps: dropped.
				i = end + 1;
				continue;
			}
		}
		if (c == '\r') {
			i++;
			continue;
		}
		if (c == '\n') {
			out += "<br/>";
			i++;
			continue;
		}
		// Inside {\p1}..\p9} vector drawing: skip the raw coordinates.
		if (drawing) {
			i++;
			continue;
		}
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '{': out += "&#123;"; break;
		case '}': out += "&#125;"; break;
		default: out += c;
		}
		i++;
	}
	if (bold) out += "</strong>";
	if (italic) out += "</em>";
	if (span_open) out += "</span>";
	// Collapse leading/trailing breaks.
	while (starts_with(out, "<br/>")) out = out.substr(5);
	while (ends_with(out, "<br/>")) out = out.substr(0, out.size() - 5);
	// Arabic / Hebrew lines into display order, letters joined (bidi.h).
	return bidi_visual_rml(out);
}

static std::vector<std::string> lines_of(const std::string& text) {
	std::vector<std::string> lines;
	std::string cur;
	for (char c : text) {
		if (c == '\n') {
			if (!cur.empty() && cur.back() == '\r') cur.pop_back();
			lines.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) lines.push_back(cur);
	return lines;
}

static std::vector<Cue> parse_srt_vtt(const std::vector<std::string>& lines) {
	std::vector<Cue> cues;
	for (size_t i = 0; i < lines.size(); i++) {
		size_t arrow = lines[i].find("-->");
		if (arrow == std::string::npos) continue;
		std::string a = trim(lines[i].substr(0, arrow));
		std::string b = trim(lines[i].substr(arrow + 3));
		size_t sp = b.find_first_of(" \t");  // VTT cue settings
		if (sp != std::string::npos) b = b.substr(0, sp);
		double start = parse_timestamp(a), end = parse_timestamp(b);
		if (start < 0 || end < 0) continue;
		std::string text;
		size_t j = i + 1;
		for (; j < lines.size(); j++) {
			if (trim(lines[j]).empty()) break;
			if (lines[j].find("-->") != std::string::npos) break;  // missing blank line
			if (!text.empty()) text += "\n";
			text += lines[j];
		}
		// A cue number right before the next timing line belongs to it, not to us.
		if (j < lines.size() && lines[j].find("-->") != std::string::npos && !text.empty()) {
			size_t nl = text.find_last_of('\n');
			std::string last = nl == std::string::npos ? text : text.substr(nl + 1);
			if (!last.empty() && last.find_first_not_of("0123456789") == std::string::npos)
				text = nl == std::string::npos ? "" : text.substr(0, nl);
		}
		i = j - 1;
		Cue c;
		c.start = start;
		c.end = end;
		c.rml = subtitle_text_to_rml(text, &c.an);
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

static std::vector<Cue> parse_ass(const std::vector<std::string>& lines) {
	std::vector<Cue> cues;
	int text_field = 9, start_field = 1, end_field = 2;
	for (auto& line : lines) {
		if (starts_with(line, "Format:") && !cues.size()) {
			auto fields = split(line.substr(7), ',');
			for (size_t k = 0; k < fields.size(); k++) {
				std::string f = lower(trim(fields[k]));
				if (f == "start") start_field = int(k);
				else if (f == "end") end_field = int(k);
				else if (f == "text") text_field = int(k);
			}
			continue;
		}
		if (!starts_with(line, "Dialogue:")) continue;
		std::string rest = line.substr(9);
		std::vector<std::string> fields;
		size_t pos = 0;
		for (int k = 0; k < text_field; k++) {
			size_t comma = rest.find(',', pos);
			if (comma == std::string::npos) break;
			fields.push_back(rest.substr(pos, comma - pos));
			pos = comma + 1;
		}
		if (int(fields.size()) < text_field) continue;
		// The Format: line is attacker-controlled; start/end fields can point
		// past what this Dialogue line actually has.
		if (start_field >= int(fields.size()) || end_field >= int(fields.size())) continue;
		Cue c;
		c.start = parse_timestamp(trim(fields[start_field]));
		c.end = parse_timestamp(trim(fields[end_field]));
		if (c.start < 0 || c.end < 0) continue;
		c.rml = subtitle_text_to_rml(rest.substr(pos), &c.an);
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

static std::vector<Cue> parse_microdvd(const std::vector<std::string>& lines) {
	// {start}{end}text with frame numbers; assume 23.976 fps unless {1}{1}fps.
	std::vector<Cue> cues;
	double fps = 23.976;
	for (auto& line : lines) {
		if (line.size() < 6 || line[0] != '{') continue;
		size_t a = line.find('}');
		if (a == std::string::npos || a + 1 >= line.size() || line[a + 1] != '{') continue;
		size_t b = line.find('}', a + 1);
		if (b == std::string::npos) continue;
		long f0 = atol(line.substr(1, a - 1).c_str());
		long f1 = atol(line.substr(a + 2, b - a - 2).c_str());
		std::string text = line.substr(b + 1);
		if (f0 == 1 && f1 == 1) {
			double v = atof(text.c_str());
			if (v > 1) fps = v;
			continue;
		}
		Cue c;
		c.start = f0 / fps;
		c.end = f1 / fps;
		c.rml = subtitle_text_to_rml(replace_all(text, "|", "\n"), &c.an);
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

std::vector<Cue> parse_subtitle_file(const std::string& text) {
	auto lines = lines_of(text);
	std::vector<Cue> cues;
	bool ass = text.find("[Script Info]") != std::string::npos || text.find("\nDialogue:") != std::string::npos;
	if (ass) cues = parse_ass(lines);
	if (cues.empty()) cues = parse_srt_vtt(lines);
	if (cues.empty()) cues = parse_microdvd(lines);
	std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
	return cues;
}

std::string cues_at(const std::vector<Cue>& cues, double t, int* an) {
	// Cues are sorted by start; find the last one that started, then look
	// back for overlapping ones (rarely more than two).
	auto it = std::upper_bound(cues.begin(), cues.end(), t, [](double v, const Cue& c) { return v < c.start; });
	std::vector<const Cue*> active;
	int looked = 0;
	while (it != cues.begin() && looked < 12) {
		--it;
		looked++;
		if (it->end > t && !it->rml.empty()) {
			// Deduplicate identical overlapping cues.
			bool dup = false;
			for (const Cue* a : active)
				if (a->rml == it->rml) { dup = true; break; }
			if (!dup) active.push_back(&(*it));
		}
	}
	if (active.empty()) return "";
	std::reverse(active.begin(), active.end()); // chronological order

	// If bottom dialogue (an <= 3) and top signs (an >= 7) overlap, prefer
	// the dialogue alignment so speech never jumps to the top of the screen.
	int chosen_an = active.back()->an;
	for (const Cue* c : active) {
		if (c->an <= 3) { chosen_an = c->an; break; }
	}
	if (an) *an = chosen_an;

	// Two simultaneous bottom speakers get dash prefixes.
	const bool multi_dialogue = active.size() == 2 && active[0]->an <= 3 && active[1]->an <= 3;
	std::string out;
	for (const Cue* c : active) {
		std::string line = (multi_dialogue && c->rml.compare(0, 2, "- ") != 0) ? ("- " + c->rml) : c->rml;
		out = out.empty() ? line : (out + "<br/>" + line);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Smart track labels
// ---------------------------------------------------------------------------

// Word-aware substring search: "sign" matches "signs & songs" but not
// "design". `hay` must already be lowered.
static bool has_word(const std::string& hay, const std::string& needle) {
	std::size_t p = hay.find(needle);
	while (p != std::string::npos) {
		const bool left_ok = p == 0 || !isalpha((unsigned char)hay[p - 1]);
		const std::size_t e = p + needle.size();
		const bool right_ok = e >= hay.size() || !isalpha((unsigned char)hay[e]);
		if (left_ok && right_ok) return true;
		p = hay.find(needle, p + 1);
	}
	return false;
}

bool sub_track_is_sdh(const std::string& label) {
	const std::string l = lower(label);
	return has_word(l, "sdh") || has_word(l, "hearing impaired") || has_word(l, "hearing-impaired") ||
	       has_word(l, "closed caption") || has_word(l, "closed-caption") || has_word(l, "cc") ||
	       l.find("[hi]") != std::string::npos || l.find("(hi)") != std::string::npos;
}

// Subtitle format from a URL/filename extension (most reliable), "" if none.
static std::string sub_format_from_hint(const std::string& hint) {
	std::string h = lower(hint);
	const std::size_t dot = h.rfind('.');
	if (dot == std::string::npos) return "";
	std::string ext = h.substr(dot);
	const std::size_t q = ext.find_first_of("?#");
	if (q != std::string::npos) ext = ext.substr(0, q);
	if (ext == ".ass" || ext == ".ssa") return "ASS";
	if (ext == ".srt") return "SRT";
	if (ext == ".vtt") return "VTT";
	if (ext == ".sub") return "MicroDVD";
	return "";
}

// Subtitle format from a free-text label ("English · ASS"), "" if none.
static std::string sub_format_from_probe(const std::string& probe) {
	if (has_word(probe, "ass") || has_word(probe, "ssa")) return "ASS";
	if (has_word(probe, "srt") || has_word(probe, "subrip")) return "SRT";
	if (has_word(probe, "vtt") || has_word(probe, "webvtt")) return "VTT";
	if (has_word(probe, "pgs")) return "PGS";
	if (has_word(probe, "vobsub")) return "VobSub";
	return "";
}

std::string format_sub_track_label(const std::string& lang_iso3, const std::string& raw_label,
                                   const std::string& url_hint) {
	const std::string lang = language_name(lang_iso3);
	const bool known_lang = lang != "Unknown";
	const std::string probe = lower(raw_label) + "\n" + lower(url_hint);
	std::string fmt = sub_format_from_hint(url_hint);
	if (fmt.empty()) fmt = sub_format_from_hint(raw_label);
	if (fmt.empty()) fmt = sub_format_from_probe(probe);
	// SDH is detected from the label only: URL path segments (".../cc/...")
	// are not reliable markers.
	const bool sdh = sub_track_is_sdh(raw_label);
	const bool signs = has_word(probe, "signs") || has_word(probe, "sign") || has_word(probe, "songs") ||
	                   has_word(probe, "karaoke");
	const bool commentary = has_word(probe, "commentary");
	const bool forced = has_word(probe, "forced");
	std::string tag;
	if (signs)
		tag = "Signs only";
	else if (commentary)
		tag = "Commentary";
	else if (!fmt.empty())
		tag = fmt;
	if (sdh) tag += (tag.empty() ? "" : ", ") + std::string("SDH");
	if (forced && !signs) tag += (tag.empty() ? "" : ", ") + std::string("forced");
	std::string base;
	if (known_lang) {
		base = lang;
	} else {
		// Keep the raw label (usually a filename) as the base, minus any
		// subtitle extension: "Dune.Part.Two.eng.srt" -> "Dune.Part.Two.eng".
		base = raw_label;
		const std::size_t slash = base.find_last_of("/\\");
		if (slash != std::string::npos) base = base.substr(slash + 1);
		const std::size_t dot = base.rfind('.');
		if (dot != std::string::npos) {
			const std::string ext = lower(base.substr(dot));
			if (ext == ".srt" || ext == ".ass" || ext == ".ssa" || ext == ".vtt" || ext == ".sub")
				base = base.substr(0, dot);
		}
		if (base.empty()) base = "Subtitles";
	}
	if (tag.empty() || lower(base) == lower(tag)) return base; // "ASS", not "ASS (ASS)"
	return base + " (" + tag + ")";
}
