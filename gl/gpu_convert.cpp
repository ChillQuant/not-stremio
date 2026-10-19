// GPU YUV->RGB conversion for the video picture. See gpu_convert.hpp.

#include "gpu_convert.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <GL/glcorearb.h>

#include "gfx/gl_program.hpp"
#include "util.h"
#include "yuv_convert.h"

namespace {

// Full-screen triangle from the vertex id alone; no vertex buffers.
const char* kVert = R"(
out vec2 v_uv;
void main() {
	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
	v_uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The conversion of src/yuv_convert.cpp in float: studio-range YUV to RGB
// with the BT.601/BT.709 matrix, or HDR10 (BT.2020, PQ, Hable tone mapping
// on the brightest channel, BT.2020->BT.709, BT.709 OETF).
const char* kFrag = R"(
#ifdef GL_ES
precision highp float;
precision highp int;
#endif
uniform sampler2D y_tex;  // packed luma: (W/4)xH RGBA8/16, 4 samples per texel
uniform sampler2D c_tex;  // packed chroma: planar U, or interleaved UV
uniform sampler2D v_tex;  // packed planar V (ignored when u_interleaved != 0)
uniform sampler2D pq_tex; // 1024x1: PQ code -> light, units of 100 nits
uniform int u_colors;     // 0 = BT.601, 1 = BT.709, 2 = HDR10 (PQ), 3 = HLG, 4 = DV P5
uniform int u_interleaved;
uniform float u_peak;     // sample scale to the 10-bit domain: 1020 or 65535
uniform float u_shift_div; // 1, or 64 for high-aligned 10-bit samples
uniform int u_deep_blacks; // 0 = Off, 1 = Low, 2 = High
uniform int u_tonemap;     // 0 = Hable (Filmic), 1 = BT.2390 (Broadcast)
uniform vec2 u_src_size;  // (p.width, p.height) in pixels
uniform int u_wide;       // 0 = 8-bit (2 bytes packed per R16 texel), 1 = 10-bit (1 uint16 per R16 texel)
uniform int u_fast;       // 1 = skip bilateral/deband (8-bit SDR fast path)
in vec2 v_uv;
out vec4 o_color;

float hable(float x) {
	const float a = 0.15, b = 0.50, c = 0.10, d = 0.20, e = 0.02, f = 0.30;
	return (x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f) - e / f;
}
// ITU-R BT.2390 EETF: the broadcast-standard PQ/HLG-to-SDR mapping.
// Shadows and mids keep their mastered level; only highlights roll off.
float pq_oetf(float nits) {
	float y = pow(clamp(nits / 10000.0, 0.0, 1.0), 0.1593017578125);
	return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
float pq_to_nits(float n) {
	float np = pow(max(n, 0.0), 1.0 / 78.84375);
	return 10000.0 * pow(max(np - 0.8359375, 0.0) / (18.8515625 - 18.6875 * np),
	                     1.0 / 0.1593017578125);
}
float bt2390_eetf(float nits) {
	const float src_peak = 1000.0;  // assumed mastering peak
	const float sdr_peak = 100.0;   // SDR reference white
	float max_lum = pq_oetf(src_peak);
	float e1 = min(pq_oetf(nits) / max_lum, 1.0);
	float max_t = pq_oetf(sdr_peak) / max_lum;
	float ks = 1.5 * max_t - 0.5;
	float e2 = e1;
	if (e1 > ks) {
		float t = (e1 - ks) / (1.0 - ks);
		float t2 = t * t, t3 = t2 * t;
		e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks
		   + (t3 - 2.0 * t2 + t) * (1.0 - ks)
		   + (-2.0 * t3 + 3.0 * t2) * max_t;
	}
	return pq_to_nits(e2 * max_lum);
}
// Deep Blacks: smooth shadow-toe curve pulling lifted greys to true black.
// vDeep = (floor, knee, slope); all zero = off.
vec3 deep_black(vec3 c, vec3 vdeep) {
	float k = vdeep.y;
	if (k <= 0.0) return c;
	vec3 u = max((c - vdeep.x) / (1.0 - vdeep.x), 0.0);
	vec3 t = min(u / k, vec3(1.0));
	vec3 t2 = t * t;
	vec3 t3 = t2 * t;
	vec3 f = k * ((t3 - 2.0 * t2 + t) * vdeep.z + (-2.0 * t3 + 3.0 * t2) + (t3 - t2));
	return mix(f, u, step(vec3(1.0), t));
}
float bt709_oetf(float l) {
	l = clamp(l, 0.0, 1.0);
	return l < 0.018 ? 4.5 * l : 1.099 * pow(l, 0.45) - 0.099;
}
float pq_lut(float e) {
	// The CPU path's 1024-entry table, indexed exactly as it does:
	// code = clamp(int(e * 1023 + 0.5), 0, 1023).
	float j = floor(clamp(e, 0.0, 1.0) * 1023.0 + 0.5);
	return texture(pq_tex, vec2((j + 0.5) / 1024.0, 0.5)).r;
}
float hlg_inv(float e) {
	// BT.2100 inverse HLG OETF: HLG code value to scene light E, nominal
	// 1000-nit peak. (The forward OETF maps the other way and must not be
	// used here.)
	e = clamp(e, 0.0, 1.0);
	return e <= 0.5 ? e * e / 3.0 : (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}

// Interleaved Gradient Noise (Jimenez) for stochastic deband sampling & 8-bit dithering.
float ign(vec2 p) {
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float sample_y10(vec2 uv, float scale) {
	int px = clamp(int(floor(uv.x * u_src_size.x)), 0, int(u_src_size.x) - 1);
	int py = clamp(int(floor(uv.y * u_src_size.y)), 0, int(u_src_size.y) - 1);
	if (u_wide != 0) {
		return texelFetch(y_tex, ivec2(px, py), 0).r * scale;
	}
	// 8-bit: each R16 texel holds 2 luma bytes (little-endian).
	uint word = uint(texelFetch(y_tex, ivec2(px >> 1, py), 0).r * 65535.0 + 0.5);
	uint b = ((px & 1) == 0) ? (word & 0xFFu) : (word >> 8);
	return float(b * 4u);
}

vec2 sample_uv10(vec2 uv, float scale) {
	int cw = int(u_src_size.x) >> 1;
	int ch = int(u_src_size.y) >> 1;
	int cx = clamp(int(floor(uv.x * float(cw))), 0, cw - 1);
	int cy = clamp(int(floor(uv.y * float(ch))), 0, ch - 1);
	if (u_wide != 0) {
		if (u_interleaved != 0) {
			float u_raw = texelFetch(c_tex, ivec2(cx * 2, cy), 0).r;
			float v_raw = texelFetch(c_tex, ivec2(cx * 2 + 1, cy), 0).r;
			return vec2(u_raw, v_raw) * scale;
		}
		float u_raw = texelFetch(c_tex, ivec2(cx, cy), 0).r;
		float v_raw = texelFetch(v_tex, ivec2(cx, cy), 0).r;
		return vec2(u_raw, v_raw) * scale;
	}
	if (u_interleaved != 0) {
		// 8-bit NV12: each R16 texel holds U in low byte, V in high byte.
		uint word = uint(texelFetch(c_tex, ivec2(cx, cy), 0).r * 65535.0 + 0.5);
		return vec2(float((word & 0xFFu) * 4u), float((word >> 8) * 4u));
	}
	// 8-bit planar: each R16 texel holds 2 horizontal U (or V) bytes.
	uint wu = uint(texelFetch(c_tex, ivec2(cx >> 1, cy), 0).r * 65535.0 + 0.5);
	uint wv = uint(texelFetch(v_tex, ivec2(cx >> 1, cy), 0).r * 65535.0 + 0.5);
	uint shift = uint((cx & 1) * 8);
	return vec2(float(((wu >> shift) & 0xFFu) * 4u), float(((wv >> shift) & 0xFFu) * 4u));
}

// Luma-guided bilateral 4:2:0 -> 4:4:4 chroma reconstruction.
// Weights the 4 nearest chroma samples by how closely their local luma matches
// the current pixel's luma y_center, preventing color bleed across sharp edges.
vec2 bilateral_chroma(vec2 uv, float y_center, float scale, vec2 c_inv_size) {
	vec2 pos = uv / c_inv_size - 0.5;
	vec2 f = fract(pos);
	vec2 base = (floor(pos) + 0.5) * c_inv_size;

	vec2 uv00 = base;
	vec2 uv10 = base + vec2(c_inv_size.x, 0.0);
	vec2 uv01 = base + vec2(0.0, c_inv_size.y);
	vec2 uv11 = base + c_inv_size;

	// Spatial bilinear weights
	float w00 = (1.0 - f.x) * (1.0 - f.y);
	float w10 = f.x * (1.0 - f.y);
	float w01 = (1.0 - f.x) * f.y;
	float w11 = f.x * f.y;

	// Luma-similarity weights (sigma = 24.0 in 10-bit code units)
	const float inv_sigma2 = 1.0 / (24.0 * 24.0);
	// Center the luma comparison on the chroma taps' true grid location.
	vec2 y_off = 0.25 / u_src_size;
	float dy00 = y_center - sample_y10(uv00 + y_off, scale);
	float dy10 = y_center - sample_y10(uv10 + y_off, scale);
	float dy01 = y_center - sample_y10(uv01 + y_off, scale);
	float dy11 = y_center - sample_y10(uv11 + y_off, scale);

	w00 /= 1.0 + dy00 * dy00 * inv_sigma2;
	w10 /= 1.0 + dy10 * dy10 * inv_sigma2;
	w01 /= 1.0 + dy01 * dy01 * inv_sigma2;
	w11 /= 1.0 + dy11 * dy11 * inv_sigma2;

	float wsum = max(w00 + w10 + w01 + w11, 1e-5);
	vec2 c = sample_uv10(uv00, scale) * w00 +
	         sample_uv10(uv10, scale) * w10 +
	         sample_uv10(uv01, scale) * w01 +
	         sample_uv10(uv11, scale) * w11;
	return c / wsum;
}

void main() {
	float scale = u_peak / u_shift_div;
	vec2 y_inv = 1.0 / u_src_size;
	vec2 c_inv = 2.0 / u_src_size;

	float y10 = sample_y10(v_uv, scale);
	float n1 = ign(gl_FragCoord.xy);
	float n2 = ign(gl_FragCoord.xy + vec2(17.0, 31.0));
	vec2 uv10;
	if (u_fast != 0) {
		// Fast path for 8-bit SDR: 4-tap bilinear chroma, no bilateral, no deband.
		vec2 pos = v_uv / c_inv - 0.5;
		vec2 f = fract(pos);
		vec2 base = (floor(pos) + 0.5) * c_inv;
		uv10 = mix(mix(sample_uv10(base, scale), sample_uv10(base + vec2(c_inv.x, 0.0), scale), f.x),
		           mix(sample_uv10(base + vec2(0.0, c_inv.y), scale), sample_uv10(base + c_inv, scale), f.x), f.y);
	} else {
		uv10 = bilateral_chroma(v_uv, y10, scale, c_inv);

		// Single-pass perceptual debanding: sample 4 stochastic rotated neighbors
		// at a 2.0..4.5 luma-texel radius. Smooths quantization bands in dark
		// gradients and skies while preserving real edges and detail.
		float angle = n1 * 6.2831853;
		float rad = (2.0 + 2.5 * n2);
		vec2 dir = vec2(cos(angle), sin(angle)) * rad * y_inv;
		vec2 perp = vec2(-dir.y, dir.x);

		float y_a = sample_y10(v_uv + dir, scale);
		float y_b = sample_y10(v_uv - dir, scale);
		float y_c = sample_y10(v_uv + perp, scale);
		float y_d = sample_y10(v_uv - perp, scale);
		float y_avg = 0.25 * (y_a + y_b + y_c + y_d);
		float y_max_diff = max(max(abs(y10 - y_a), abs(y10 - y_b)),
		                       max(abs(y10 - y_c), abs(y10 - y_d)));
		// Threshold: 10.0 in 10-bit codes (~2.5 steps in 8-bit). Smoothly backs off on real edges.
		float deband_w = clamp(1.0 - y_max_diff * (1.0 / 10.0), 0.0, 1.0);
		y10 = mix(y10, y_avg, deband_w * 0.75);

		// Light chroma debanding on flat regions
		vec2 uv_a = sample_uv10(v_uv + dir * 1.5, scale);
		vec2 uv_b = sample_uv10(v_uv - dir * 1.5, scale);
		vec2 uv_avg = 0.5 * (uv_a + uv_b);
		vec2 uv_diff = max(abs(uv10 - uv_a), abs(uv10 - uv_b));
		vec2 uv_w = clamp(1.0 - uv_diff * (1.0 / 8.0), 0.0, 1.0) * deband_w;
		uv10 = mix(uv10, uv_avg, uv_w * 0.65);
	}

	float u10 = uv10.x;
	float v10 = uv10.y;

	vec3 rgb;
	if (u_colors == 2) {
		float y = (y10 - 64.0) / 876.0;
		float cb = (u10 - 512.0) / 896.0;
		float cr = (v10 - 512.0) / 896.0;
		vec3 pq = vec3(pq_lut(y + 1.4746 * cr), pq_lut(y - 0.16455 * cb - 0.57135 * cr),
		             pq_lut(y + 1.8814 * cb))
		          * 2.5;
		vec3 t;
		t.r = 1.6605 * pq.r - 0.5876 * pq.g - 0.0728 * pq.b;
		t.g = -0.1246 * pq.r + 1.1329 * pq.g - 0.0083 * pq.b;
		t.b = -0.0182 * pq.r - 0.1006 * pq.g + 1.1187 * pq.b;
		// Desaturate out-of-gamut chroma back along the luma ray before
		// tone mapping, then map with an 80/20 max-channel/luma blend so
		// saturated highlights don't wash out.
		float lum = max(0.2126 * t.r + 0.7152 * t.g + 0.0722 * t.b, 1e-6);
		float min_c = min(min(t.r, t.g), t.b);
		if (min_c < 0.0) {
			t = lum + (t - lum) * (lum / (lum - min_c + 1e-6));
		}
		if (u_tonemap == 1) {
			float sig = max(max(t.r, t.g), max(t.b, 1e-6));
			float mapped_sig = bt2390_eetf(sig * 100.0) / 100.0;
			float mapped_lum = bt2390_eetf(lum * 100.0) / 100.0;
			t *= mix(mapped_sig / sig, mapped_lum / lum, 0.20);
		} else {
			float sig = max(max(t.r, t.g), max(t.b, 1e-6));
			t *= hable(sig) / hable(10.0) / sig;
		}
		rgb = vec3(bt709_oetf(t.r), bt709_oetf(t.g), bt709_oetf(t.b));
	} else if (u_colors == 3) {
		float y = (y10 - 64.0) / 876.0;
		float cb = (u10 - 512.0) / 896.0;
		float cr = (v10 - 512.0) / 896.0;
		vec3 hlg = vec3(hlg_inv(y + 1.4746 * cr), hlg_inv(y - 0.16455 * cb - 0.57135 * cr),
		             hlg_inv(y + 1.8814 * cb))
		          * 10.0;
		vec3 t;
		t.r = 1.6605 * hlg.r - 0.5876 * hlg.g - 0.0728 * hlg.b;
		t.g = -0.1246 * hlg.r + 1.1329 * hlg.g - 0.0083 * hlg.b;
		t.b = -0.0182 * hlg.r - 0.1006 * hlg.g + 1.1187 * hlg.b;
		// Desaturate out-of-gamut chroma back along the luma ray before
		// tone mapping, then map with an 80/20 max-channel/luma blend so
		// saturated highlights don't wash out.
		float lum = max(0.2126 * t.r + 0.7152 * t.g + 0.0722 * t.b, 1e-6);
		float min_c = min(min(t.r, t.g), t.b);
		if (min_c < 0.0) {
			t = lum + (t - lum) * (lum / (lum - min_c + 1e-6));
		}
		if (u_tonemap == 1) {
			float sig = max(max(t.r, t.g), max(t.b, 1e-6));
			float mapped_sig = bt2390_eetf(sig * 100.0) / 100.0;
			float mapped_lum = bt2390_eetf(lum * 100.0) / 100.0;
			t *= mix(mapped_sig / sig, mapped_lum / lum, 0.20);
		} else {
			float sig = max(max(t.r, t.g), max(t.b, 1e-6));
			t *= hable(sig) / hable(10.0) / sig;
		}
		rgb = vec3(bt709_oetf(t.r), bt709_oetf(t.g), bt709_oetf(t.b));
	} else if (u_colors == 4) {
		vec3 ipt = vec3(y10, u10, v10) / 1023.0;
		vec3 lmsp;
		lmsp.r = ipt.r + 1.79956055 * (ipt.b - 0.5);
		lmsp.g = ipt.r + 0.21411133 * (ipt.g - 0.5) + 0.53503418 * (ipt.b - 0.5);
		lmsp.b = ipt.r + 2.12060547 * (ipt.g - 0.5);
		vec3 lms = vec3(pq_lut(lmsp.r), pq_lut(lmsp.g), pq_lut(lmsp.b));
		vec3 tmp;
		tmp.r = 0.35675049 * lms.r + 0.59216309 * lms.g + 0.05108643 * lms.b;
		tmp.g = 0.15673828 * lms.r + 0.74804688 * lms.g + 0.09527588 * lms.b;
		tmp.b = 0.0 * lms.r + 0.04144287 * lms.g + 0.95855713 * lms.b;
		vec3 dv;
		dv.r = 3.06441879 * tmp.r - 2.16597676 * tmp.g + 0.10155818 * tmp.b;
		dv.g = -0.65612108 * tmp.r + 1.78554118 * tmp.g - 0.12943749 * tmp.b;
		dv.b = 0.01736321 * tmp.r - 0.04725154 * tmp.g + 1.03004253 * tmp.b;
		dv *= 2.5;
		vec3 t;
		t.r = 1.6605 * dv.r - 0.5876 * dv.g - 0.0728 * dv.b;
		t.g = -0.1246 * dv.r + 1.1329 * dv.g - 0.0083 * dv.b;
		t.b = -0.0182 * dv.r - 0.1006 * dv.g + 1.1187 * dv.b;
		// Desaturate out-of-gamut chroma back along the luma ray before
		// tone mapping, then map with an 80/20 max-channel/luma blend so
		// saturated highlights don't wash out.
		float lum = max(0.2126 * t.r + 0.7152 * t.g + 0.0722 * t.b, 1e-6);
		float min_c = min(min(t.r, t.g), t.b);
		if (min_c < 0.0) {
			t = lum + (t - lum) * (lum / (lum - min_c + 1e-6));
		}
		if (u_tonemap == 1) {
			float sig = max(max(t.r, t.g), max(t.b, 1e-6));
			float mapped_sig = bt2390_eetf(sig * 100.0) / 100.0;
			float mapped_lum = bt2390_eetf(lum * 100.0) / 100.0;
			t *= mix(mapped_sig / sig, mapped_lum / lum, 0.20);
		} else {
			float sig = max(max(t.r, t.g), max(t.b, 1e-6));
			t *= hable(sig) / hable(10.0) / sig;
		}
		rgb = vec3(bt709_oetf(t.r), bt709_oetf(t.g), bt709_oetf(t.b));
	} else {
		float y8 = y10 * 0.25;
		float u8 = u10 * 0.25 - 128.0;
		float v8 = v10 * 0.25 - 128.0;
		float c = 298.0 * (y8 - 16.0) + 128.0;
		vec4 m = u_colors == 0 ? vec4(409.0, 100.0, 208.0, 516.0)   // BT.601
		                       : vec4(459.0, 55.0, 136.0, 541.0);  // BT.709
		// /256 is the integer >>8 of put_sdr(); /255 puts it in 0..1.
		rgb = vec3(c + m.x * v8, c - m.y * u8 - m.z * v8, c + m.w * u8) / (256.0 * 255.0);
	}
	// Deep Blacks: pull lifted shadows to true black (Off/Low/High).
	vec3 vdeep = u_deep_blacks == 2 ? vec3(0.04, 0.25, 0.30)
	           : u_deep_blacks == 1 ? vec3(0.02, 0.15, 0.50)
	           : vec3(0.0);
	rgb = deep_black(rgb, vdeep);
	// Sub-LSB triangular dither (-0.5..+0.5 / 255.0) before GL_RGBA8 target
	// quantization so 10-bit tone-mapped gradients never re-band in the 8-bit framebuffer.
	float dither = (n1 + n2 - 1.0) * (0.5 / 255.0);
	o_color = vec4(clamp(rgb + vec3(dither), 0.0, 1.0), 1.0);
}
)";

// PQ code value (0..1) to light, units of 100 nits: src/yuv_convert.cpp.
float pq_eotf(float e) {
	const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f,
	            c3 = 18.6875f;
	float p = std::pow(std::max(e, 0.0f), 1.0f / m2);
	float l = std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
	return l * 100.0f;
}

void tex_params() {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

}  // namespace

bool GpuYuv::gl_ok(const char* what) {
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
		if (!logged_fail_) {
			logged_fail_ = true;
			dlog("gpu: %s: GL error 0x%x; falling back to the CPU path", what, unsigned(e));
		}
		return false;
	}
	return true;
}

bool GpuYuv::ensure() {
	if (program_) return true;
	if (logged_fail_) return false;  // tried once; don't spam the log
	auto fail = [&] {
		if (program_) glDeleteProgram(program_);
		program_ = 0;
		logged_fail_ = true;
		dlog("gpu: YUV shaders unavailable; staying on the CPU path");
		return false;
	};
	program_ = hui::gfx::build_program("yuv", kVert, kFrag);
	if (!program_) return fail();  // build_program logs the info log itself
	u_y_tex_ = glGetUniformLocation(program_, "y_tex");
	u_c_tex_ = glGetUniformLocation(program_, "c_tex");
	u_v_tex_ = glGetUniformLocation(program_, "v_tex");
	u_pq_tex_ = glGetUniformLocation(program_, "pq_tex");
	u_colors_ = glGetUniformLocation(program_, "u_colors");
	u_interleaved_ = glGetUniformLocation(program_, "u_interleaved");
	u_peak_ = glGetUniformLocation(program_, "u_peak");
	u_shift_div_ = glGetUniformLocation(program_, "u_shift_div");
	u_deep_blacks_ = glGetUniformLocation(program_, "u_deep_blacks");
	u_tonemap_ = glGetUniformLocation(program_, "u_tonemap");
	u_src_size_ = glGetUniformLocation(program_, "u_src_size");
	u_wide_ = glGetUniformLocation(program_, "u_wide");
	u_fast_ = glGetUniformLocation(program_, "u_fast");
	if (u_y_tex_ < 0 || u_c_tex_ < 0 || u_v_tex_ < 0 || u_pq_tex_ < 0 || u_colors_ < 0 ||
	    u_interleaved_ < 0 || u_peak_ < 0 || u_shift_div_ < 0 || u_deep_blacks_ < 0 ||
	    u_tonemap_ < 0 || u_src_size_ < 0 || u_wide_ < 0 || u_fast_ < 0)
		return fail();
	glGenVertexArrays(1, &vao_);
	glGenFramebuffers(1, &fbo_);
	// The PQ curve as a 1D lookup, like the CPU path's 1024-entry table.
	float pq[1024];
	for (int i = 0; i < 1024; i++) pq[i] = pq_eotf(i / 1023.0f);
	glGenTextures(1, &tex_pq_);
	glBindTexture(GL_TEXTURE_2D, tex_pq_);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1024, 1, 0, GL_RED, GL_FLOAT, pq);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (!gl_ok("pq lut")) return fail();
	dlog("gpu: YUV conversion ready");
	return true;
}

bool GpuYuv::upload_planes(const Player::Picture& pic) {
	const YuvPicture& p = pic.planes;
	// Width requirements for the R16 packing:
	// - interleaved (NV12): even width (width/2 texels)
	// - planar: width divisible by 4 (width/4 texels per plane)
	const bool ok_w = p.interleaved ? (p.width & 1) == 0 : (p.width & 3) == 0;
	if (!p.y || !p.u || (!p.interleaved && !p.v) || !ok_w) return false;
	const int cw = p.width / 2, ch = p.height / 2;
	const bool wide = p.wide;
	// Every plane is uploaded as GL_R16 (GL_RED, GL_UNSIGNED_SHORT) to hit
	// ps5-opengl's 1.24 ms direct-copy fast path:
	//   - 8-bit (!wide): y_tw = width/2 (2 luma bytes/texel);
	//                    c_tw = cw (interleaved UV pair/texel) or cw/2 (planar).
	//   - 10-bit (wide): y_tw = width (1 uint16/texel);
	//                    c_tw = cw*2 (interleaved U,V uint16s) or cw (planar).
	const int y_tw = wide ? p.width : (p.width / 2);
	const int c_tw = wide ? (p.interleaved ? cw * 2 : cw)
	                      : (p.interleaved ? cw : cw / 2);
	if (!tex_y_ || py_w_ != y_tw || py_h_ != p.height || pc_w_ != c_tw || pc_h_ != ch ||
	    wide_ != wide || interleaved_ != p.interleaved) {
		for (GLuint t : {tex_y_, tex_c_, tex_v_})
			if (t) glDeleteTextures(1, &t);
		tex_y_ = tex_c_ = tex_v_ = 0;
		auto alloc = [&](GLuint& t, int w, int h) {
			glGenTextures(1, &t);
			glBindTexture(GL_TEXTURE_2D, t);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, w, h, 0, GL_RED, GL_UNSIGNED_SHORT, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		};
		alloc(tex_y_, y_tw, p.height);
		alloc(tex_c_, c_tw, ch);
		if (!p.interleaved) alloc(tex_v_, c_tw, ch);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (!gl_ok("plane alloc")) return false;
		py_w_ = y_tw;
		py_h_ = p.height;
		pc_w_ = c_tw;
		pc_h_ = ch;
		wide_ = wide;
		interleaved_ = p.interleaved;
		frames_since_alloc_ = 0;  // re-verify the upload path after realloc
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	auto upload = [&](GLuint t, int w, int h, const uint8_t* data) {
		glBindTexture(GL_TEXTURE_2D, t);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_SHORT, data);
	};
	upload(tex_y_, y_tw, p.height, p.y);
	upload(tex_c_, c_tw, ch, p.u);
	if (!p.interleaved) upload(tex_v_, c_tw, ch, p.v);
	glBindTexture(GL_TEXTURE_2D, 0);
	// Only pay for glGetError during the first 60 frames after (re)alloc;
	// afterwards the path is proven and the check is pure overhead.
	if (frames_since_alloc_ < 60) {
		frames_since_alloc_++;
		return gl_ok("plane upload");
	}
	return true;
}

bool GpuYuv::render(const Player::Picture& pic, std::uint32_t& target, int& tw, int& th) {
	if (!ready() || !pic.yuv) return false;
	// Render at source resolution, not the output box. The upscaler (or the
	// shell's draw) handles scaling to display size. This lets the upscaler
	// engagement check (video_w < display_w) work correctly.
	const int sw = pic.src_w > 0 ? pic.src_w : pic.w;
	const int sh = pic.src_h > 0 ? pic.src_h : pic.h;
	if (sw <= 0 || sh <= 0) return false;
	if (!upload_planes(pic)) return false;
	GLuint tgt = target;
	if (!tgt || tw != sw || th != sh) {
		if (tgt) glDeleteTextures(1, &tgt);
		glGenTextures(1, &tgt);
		glBindTexture(GL_TEXTURE_2D, tgt);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		tex_params();
		glBindTexture(GL_TEXTURE_2D, 0);
		if (!gl_ok("target alloc")) {
			if (tgt) glDeleteTextures(1, &tgt);
			target = 0;
			tw = th = 0;
			return false;
		}
		target = tgt;
		tw = sw;
		th = sh;
		fbo_checked_ = false;  // new target texture: re-verify completeness once
	}
	GLint vp[4];
	glGetIntegerv(GL_VIEWPORT, vp);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tgt, 0);
	const GLenum db = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &db);
	if (!fbo_checked_ && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (!logged_fail_) {
			logged_fail_ = true;
			dlog("gpu: framebuffer incomplete; falling back to the CPU path");
		}
		if (tgt) glDeleteTextures(1, &tgt);
		target = 0;
		tw = th = 0;
		return false;
	}
	fbo_checked_ = true;
	glViewport(0, 0, sw, sh);
	glUseProgram(program_);
	glUniform1i(u_y_tex_, 0);
	glUniform1i(u_c_tex_, 1);
	glUniform1i(u_v_tex_, 2);
	glUniform1i(u_pq_tex_, 3);
	glUniform1i(u_colors_, pic.colors == YuvColors::Hdr10 ? 2 : pic.colors == YuvColors::Hlg ? 3 : pic.colors == YuvColors::DoviP5 ? 4 : pic.colors == YuvColors::Bt601 ? 0 : 1);
	glUniform1i(u_interleaved_, pic.planes.interleaved ? 1 : 0);
	glUniform1f(u_peak_, pic.planes.wide ? 65535.0f : 1020.0f);
	glUniform1f(u_shift_div_, pic.planes.wide && pic.planes.shift ? 64.0f : 1.0f);
	glUniform1i(u_deep_blacks_, deep_blacks_);
	glUniform1i(u_tonemap_, tonemap_);
	glUniform1i(u_wide_, pic.planes.wide ? 1 : 0);
	glUniform2f(u_src_size_, float(pic.planes.width), float(pic.planes.height));
	// Fast path for 8-bit SDR: skip bilateral/deband only when the upscaler is
	// off and Deep Blacks is off. When the upscaler IS engaged, the source
	// feeds a magnified view, so keep bilateral 4:4:4 chroma + debanding
	// even on 8-bit SDR (they become visible there).
	const bool is_hdr = pic.colors == YuvColors::Hdr10 || pic.colors == YuvColors::Hlg ||
	                    pic.colors == YuvColors::DoviP5;
	const int fast = (!pic.planes.wide && !is_hdr && !hq_chroma_ && deep_blacks_ == 0) ? 1 : 0;
	glUniform1i(u_fast_, fast);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex_y_);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, tex_c_);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, tex_v_);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, tex_pq_);
	glBindVertexArray(vao_);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	bool drawn = gl_ok("yuv draw");
	// Leave the GL state as the toolkit expects it.
	glBindVertexArray(0);
	for (int unit = 3; unit >= 0; --unit) {
		glActiveTexture(GL_TEXTURE0 + unit);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	if (!drawn) {
		if (tgt) glDeleteTextures(1, &tgt);
		target = 0;
		tw = th = 0;
		return false;
	}
	return true;
}
