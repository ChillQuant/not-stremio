#include "yuv_convert.h"

#include <cmath>
#include <mutex>

namespace {

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// A 10-bit sample (8-bit samples are scaled up).
inline int sample(const YuvPicture& p, const uint8_t* row, int x) {
	return p.wide ? (reinterpret_cast<const uint16_t*>(row)[x] >> p.shift) & 1023 : row[x] << 2;
}

// ---------------------------------------------------------------------------
// SDR: integer studio-range matrices, x256, on 8-bit values

struct Matrix {
	int rv, gu, gv, bu;
};
const Matrix kBt709{459, 55, 136, 541}, kBt601{409, 100, 208, 516};

inline void put_sdr(uint8_t* o, int y10, int u10, int v10, const Matrix& m) {
	int c = 298 * ((y10 >> 2) - 16) + 128;
	int u = (u10 >> 2) - 128, v = (v10 >> 2) - 128;
	o[0] = clamp8((c + m.rv * v) >> 8);
	o[1] = clamp8((c - m.gu * u - m.gv * v) >> 8);
	o[2] = clamp8((c + m.bu * u) >> 8);
	o[3] = 255;
}

// ---------------------------------------------------------------------------
// HDR10

// Brightest light, in units of 100 nits: 1000 nits, what HDR10 films and
// series are mastered for (it matched FFmpeg's tonemap reading the stream's
// mastering metadata, on "What If" S02E03).
const float kPeak = 10.0f;
// Light scale before tone mapping. 1.0 came out about half as bright as
// FFmpeg's tonemap; 2.5 matched its brightness on test frames ("What If"
// S02E03, measured), with natural colours.
const float kGain = 2.5f;
const int kOutLutSize = 4096;

float pq_eotf(float e) {  // PQ code value (0..1) to light, units of 100 nits
	const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
	float p = std::pow(std::max(e, 0.0f), 1.0f / m2);
	float l = std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
	return l * 10000.0f / 100.0f;
}

float hable(float x) {
	const float a = 0.15f, b = 0.50f, c = 0.10f, d = 0.20f, e = 0.02f, f = 0.30f;
	return (x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f) - e / f;
}

float bt709_oetf(float l) {
	l = std::min(std::max(l, 0.0f), 1.0f);
	return l < 0.018f ? 4.5f * l : 1.099f * std::pow(l, 0.45f) - 0.099f;
}

struct HdrTables {
	float pq[1024];            // PQ code (10-bit) to light
	uint8_t out[kOutLutSize];  // SDR light 0..1, indexed by sqrt(light), to 8-bit BT.709
	float hable_norm;
	HdrTables() {
		for (int i = 0; i < 1024; i++) pq[i] = pq_eotf(i / 1023.0f);
		for (int i = 0; i < kOutLutSize; i++) {
			float s = i / float(kOutLutSize - 1);
			out[i] = uint8_t(std::lround(bt709_oetf(s * s) * 255.0f));
		}
		hable_norm = 1.0f / hable(kPeak);
	}
};

const HdrTables& hdr_tables() {
	static HdrTables* t = new HdrTables();  // built once, never freed
	return *t;
}

inline uint8_t hdr_out(const HdrTables& t, float l) {  // l: SDR light, 0..1
	if (l <= 0) return t.out[0];
	int i = int(std::sqrt(l) * (kOutLutSize - 1) + 0.5f);
	return t.out[i < kOutLutSize ? i : kOutLutSize - 1];
}

// ---------------------------------------------------------------------------
// HLG (BT.2100)

// Inverse HLG OETF (BT.2100): HLG code value (0..1) to scene light E,
// normalized to a nominal 1000-nit peak. NOTE: this is the *inverse* OETF
// (E' -> E): E = E'^2/3 below the 0.5 knee, E = (exp((E'-c)/a)+b)/12 above
// (a=0.17883277, b=0.28466892, c=0.55991073); both branches meet at
// E'(0.5) -> E(1/12). (The forward OETF, sqrt(3E') etc., maps the other
// way and must NOT be used here.)
inline float hlg_oetf_inv(float e) {
	e = e < 0.0f ? 0.0f : e > 1.0f ? 1.0f : e;
	if (e <= 0.5f) return e * e / 3.0f;
	return (std::exp((e - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
}

inline void put_hlg(const HdrTables& t, uint8_t* o, int y10, int u10, int v10) {
	// Same pipeline as put_hdr, but the inverse HLG OETF instead of the PQ
	// table. E is scene light with a nominal 1000-nit peak, so E * 10 is
	// light in units of 100 nits — the same units pq_eotf() produces.
	// No kGain: HLG's 50% knee lands at ~50% SDR through the Hable curve,
	// which is HLG's SDR backward-compatibility design (the 2.5x PQ gain
	// was specific to PQ's absolute light mapping).
	float y = (y10 - 64) * (1.0f / 876.0f);
	float cb = (u10 - 512) * (1.0f / 896.0f), cr = (v10 - 512) * (1.0f / 896.0f);
	float rp = y + 1.4746f * cr;
	float gp = y - 0.16455f * cb - 0.57135f * cr;
	float bp = y + 1.8814f * cb;
	float r = hlg_oetf_inv(rp) * 10.0f, g = hlg_oetf_inv(gp) * 10.0f, b = hlg_oetf_inv(bp) * 10.0f;
	// BT.2020 to BT.709 primaries, in light
	float r7 = 1.6605f * r - 0.5876f * g - 0.0728f * b;
	float g7 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
	float b7 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
	// Tone mapping on the brightest channel, the same factor for all three
	// (as FFmpeg's tonemap filter): keeps hue and saturation.
	float sig = std::max(std::max(r7, g7), std::max(b7, 1e-6f));
	float k = hable(sig) * t.hable_norm / sig;
	r7 *= k;
	g7 *= k;
	b7 *= k;
	o[0] = hdr_out(t, r7);
	o[1] = hdr_out(t, g7);
	o[2] = hdr_out(t, b7);
	o[3] = 255;
}

inline void put_hdr(const HdrTables& t, uint8_t* o, int y10, int u10, int v10) {	// BT.2020 non-constant luminance, 10-bit studio range, to R'G'B' (0..1)
	float y = (y10 - 64) * (1.0f / 876.0f);
	float cb = (u10 - 512) * (1.0f / 896.0f), cr = (v10 - 512) * (1.0f / 896.0f);
	float rp = y + 1.4746f * cr;
	float gp = y - 0.16455f * cb - 0.57135f * cr;
	float bp = y + 1.8814f * cb;
	auto code = [](float e) { int i = int(e * 1023.0f + 0.5f); return i < 0 ? 0 : i > 1023 ? 1023 : i; };
	float r = t.pq[code(rp)] * kGain, g = t.pq[code(gp)] * kGain, b = t.pq[code(bp)] * kGain;
	// BT.2020 to BT.709 primaries, in light
	float r7 = 1.6605f * r - 0.5876f * g - 0.0728f * b;
	float g7 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
	float b7 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
	// Tone mapping on the brightest channel, the same factor for all three
	// (as FFmpeg's tonemap filter): keeps hue and saturation.
	float sig = std::max(std::max(r7, g7), std::max(b7, 1e-6f));
	float k = hable(sig) * t.hable_norm / sig;
	r7 *= k;
	g7 *= k;
	b7 *= k;
	o[0] = hdr_out(t, r7);
	o[1] = hdr_out(t, g7);
	o[2] = hdr_out(t, b7);
	o[3] = 255;
}

// ---------------------------------------------------------------------------
// Dolby Vision Profile 5
//
// The planes hold reshaped IPT-PQ-c2, not YCbCr (P5 has no HDR10 base
// layer). This mirrors the GPU shader's u_colors == 4 branch exactly:
// identity reshaping (v1, no per-frame RPU curves) with Dolby's reference
// matrices (FFmpeg's ff_dovi_color_default), then the HDR10 tail (gain,
// BT.2020->BT.709, Hable, BT.709 OETF). P5 is full-range: the 10-bit codes
// normalize by /1023, not the studio-range formula put_hdr uses.
inline void put_dovi(const HdrTables& t, uint8_t* o, int y10, int u10, int v10) {
	float i = y10 * (1.0f / 1023.0f), ct = u10 * (1.0f / 1023.0f), cp = v10 * (1.0f / 1023.0f);
	// YCC->LMS' (ff_dovi_color_default ycc_to_rgb_matrix), neutral offsets {0, 0.5, 0.5}.
	float lr = 1.16882324f * (i - 0.0f) + 0.0f * (ct - 0.5f) + 1.79956055f * (cp - 0.5f);
	float lg = 1.16882324f * (i - 0.0f) + 0.21411133f * (ct - 0.5f) + 0.53503418f * (cp - 0.5f);
	float lb = 1.16882324f * (i - 0.0f) + 2.12060547f * (ct - 0.5f) + 0.0f * (cp - 0.5f);
	// PQ EOTF -> linear LMS, in units of 100 nits like pq_eotf().
	auto code = [](float e) { int i = int(e * 1023.0f + 0.5f); return i < 0 ? 0 : i > 1023 ? 1023 : i; };
	float l0 = t.pq[code(lr)], l1 = t.pq[code(lg)], l2 = t.pq[code(lb)];
	// RGB->LMS (ff_dovi_color_default rgb_to_lms_matrix)...
	float t0 = 0.35675049f * l0 + 0.59216309f * l1 + 0.05108643f * l2;
	float t1 = 0.15673828f * l0 + 0.74804688f * l1 + 0.09527588f * l2;
	float t2 = 0.0f * l0 + 0.04144287f * l1 + 0.95855713f * l2;
	// ...then HPE LMS -> linear BT.2020 RGB (libplacebo dovi_lms2rgb).
	float r = 3.06441879f * t0 - 2.16597676f * t1 + 0.10155818f * t2;
	float g = -0.65612108f * t0 + 1.78554118f * t1 - 0.12943749f * t2;
	float b = 0.01736321f * t0 - 0.04725154f * t1 + 1.03004253f * t2;
	// Now ordinary HDR10: the same kGain as put_hdr (PQ is PQ), then the
	// BT.2020->BT.709 matrix, Hable tone map, BT.709 OETF.
	r *= kGain;
	g *= kGain;
	b *= kGain;
	float r7 = 1.6605f * r - 0.5876f * g - 0.0728f * b;
	float g7 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
	float b7 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
	float sig = std::max(std::max(r7, g7), std::max(b7, 1e-6f));
	float k = hable(sig) * t.hable_norm / sig;
	r7 *= k;
	g7 *= k;
	b7 *= k;
	o[0] = hdr_out(t, r7);
	o[1] = hdr_out(t, g7);
	o[2] = hdr_out(t, b7);
	o[3] = 255;
}

}  // namespace

void yuv_to_rgba(const YuvPicture& p, YuvColors colors, int scale, uint8_t* out, int out_w, int row_begin,
                 int row_end) {
	const HdrTables* t = (colors == YuvColors::Hdr10 || colors == YuvColors::Hlg || colors == YuvColors::DoviP5)
	                           ? &hdr_tables()
	                           : nullptr;
	const Matrix& m = colors == YuvColors::Bt601 ? kBt601 : kBt709;
	for (int oy = row_begin; oy < row_end; oy++) {
		uint8_t* o = out + size_t(oy) * out_w * 4;
		int sy = scale == 2 ? oy * 2 : oy;
		const uint8_t* y0 = p.y + size_t(sy) * p.y_stride;
		const uint8_t* y1 = scale == 2 ? y0 + p.y_stride : y0;
		const uint8_t* cu = p.u + size_t(sy / 2) * p.c_stride;
		const uint8_t* cv = p.interleaved ? cu : p.v + size_t(sy / 2) * p.c_stride;
		for (int ox = 0; ox < out_w; ox++, o += 4) {
			int x = scale == 2 ? ox * 2 : ox;
			int yv = scale == 2
			             ? (sample(p, y0, x) + sample(p, y0, x + 1) + sample(p, y1, x) + sample(p, y1, x + 1) + 2) >> 2
			             : sample(p, y0, x);
			int cx = x / 2;
			int uv = p.interleaved ? sample(p, cu, cx * 2) : sample(p, cu, cx);
			int vv = p.interleaved ? sample(p, cv, cx * 2 + 1) : sample(p, cv, cx);
			if (t) {
				if (colors == YuvColors::Hlg) put_hlg(*t, o, yv, uv, vv);
				else if (colors == YuvColors::DoviP5) put_dovi(*t, o, yv, uv, vv);
				else put_hdr(*t, o, yv, uv, vv);
			} else put_sdr(o, yv, uv, vv, m);
		}
	}
}
