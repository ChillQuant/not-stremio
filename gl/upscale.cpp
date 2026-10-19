// Video upscaling: FSR1 EASU+RCAS. See upscale.hpp.
//
// The FSR1 shaders are EVO Player's clean-room reimplementation (GPL-3.0),
// embedded via upscale_shaders.hpp. They use texelFetch directly — no
// textureGather, no CPU-side constants.

#include "upscale.hpp"

#include <cmath>
#include <cstring>
#include <string>

#include <GL/glcorearb.h>

#include "gfx/gl_program.hpp"
#include "upscale_shaders.hpp"
#include "util.h"

namespace {

void tex_params() {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// Cinema HQ: separable anti-ringing bicubic spatial upsampling. The bicubic
// is split per axis into three taps (tc0/tc12/tc3); the middle tap's
// fractional position lets hardware bilinear combine the w1+w2 pair, so
// only 9 texture() calls cover the full 4x4 kernel. De-ringed by clamping
// to the 2x2 neighborhood min/max with an 0.85 blend toward the clamp.
//
// I/O and uniform naming match the other shaders in upscale_shaders.hpp:
// u_src sampler, vUV in, out_color out. The video texture is GL_LINEAR
// (hui GlBatch::create_texture), which the fractional bilinear taps need.
static const char* kCinemaFrag = R"UPCINE(
uniform sampler2D u_src;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

void main() {
	vec2 src_size = vec2(textureSize(u_src, 0));
	vec2 pos = vUV * src_size - 0.5;
	vec2 f = fract(pos);
	vec2 base = (floor(pos) + 0.5) / src_size;
	vec2 inv = 1.0 / src_size;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec3 c00 = texture(u_src, base).rgb;
	vec3 c10 = texture(u_src, base + vec2(inv.x, 0.0)).rgb;
	vec3 c01 = texture(u_src, base + vec2(0.0, inv.y)).rgb;
	vec3 c11 = texture(u_src, base + inv).rgb;
	vec3 min_c = min(min(c00, c10), min(c01, c11));
	vec3 max_c = max(max(c00, c10), max(c01, c11));
	vec2 w12 = w1 + w2;
	vec2 tc0 = base - inv;
	vec2 tc12 = base + (w2 / max(w12, vec2(1e-5))) * inv;
	vec2 tc3 = base + 2.0 * inv;
	vec3 sum =
		(texture(u_src, vec2(tc12.x, tc0.y)).rgb * w12.x) * w0.y +
		(texture(u_src, vec2(tc0.x, tc12.y)).rgb * w0.x +
		 texture(u_src, vec2(tc12.x, tc12.y)).rgb * w12.x +
		 texture(u_src, vec2(tc3.x, tc12.y)).rgb * w3.x) * w12.y +
		(texture(u_src, vec2(tc12.x, tc3.y)).rgb * w12.x) * w3.y;
	sum /= (w12.x * w0.y + (w0.x + w12.x + w3.x) * w12.y + w12.x * w3.y);
	out_color = vec4(mix(sum, clamp(sum, min_c, max_c), 0.85), 1.0);
}
)UPCINE";

// Anime4K with an adjustable line-push strength (u_push uniform). Same
// line-aware single-pass approach as kUpscaleAnime4kFrag in
// upscale_shaders.hpp, which hardcodes the push and has no uniform; this
// variant lives here instead (upscale_shaders.hpp is not edited). The
// pushed RGB is clamped to the 3x3 neighborhood min/max before the
// gradient-reversal sharpen, which is also clamped, so no ringing or halos
// form. u_push = 0.25 + 0.75 * sharpness from the caller.
static const char* kAnime4kPushFrag = R"UP4KP(
uniform sampler2D u_src;
uniform float u_push;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

float luma(vec3 c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

void main() {
	vec2 px = 1.0 / vec2(textureSize(u_src, 0));
	// 3x3 RGB neighborhood.
	vec3 ntl = texture(u_src, vUV + vec2(-px.x, -px.y)).rgb;
	vec3 ntc = texture(u_src, vUV + vec2(0.0, -px.y)).rgb;
	vec3 ntr = texture(u_src, vUV + vec2(px.x, -px.y)).rgb;
	vec3 nml = texture(u_src, vUV + vec2(-px.x, 0.0)).rgb;
	vec3 nmc = texture(u_src, vUV).rgb;
	vec3 nmr = texture(u_src, vUV + vec2(px.x, 0.0)).rgb;
	vec3 nbl = texture(u_src, vUV + vec2(-px.x, px.y)).rgb;
	vec3 nbc = texture(u_src, vUV + vec2(0.0, px.y)).rgb;
	vec3 nbr = texture(u_src, vUV + vec2(px.x, px.y)).rgb;
	// 3x3 min/max: clamps the pushed sample and the sharpened result.
	vec3 mn = min(min(min(ntl, ntc), min(ntr, nml)), min(min(nmc, nmr), min(nbl, nbc)));
	mn = min(mn, nbr);
	vec3 mx = max(max(max(ntl, ntc), max(ntr, nml)), max(max(nmc, nmr), max(nbl, nbc)));
	mx = max(mx, nbr);
	// Sobel gradients -> dominant line direction.
	float tl = luma(ntl), tc = luma(ntc), tr = luma(ntr);
	float ml = luma(nml), mc = luma(nmc), mr = luma(nmr);
	float bl = luma(nbl), bc = luma(nbc), br = luma(nbr);
	float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
	float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
	float g = length(vec2(gx, gy));
	// Push the sample point perpendicular to the gradient (along the line)
	// proportional to edge strength, scaled by u_push: thins lines without
	// ringing.
	vec2 dir = g > 1e-4 ? normalize(vec2(-gy, gx)) : vec2(0.0);
	float push = clamp(g * 2.0, 0.0, 0.45) * u_push;
	vec3 c = clamp(texture(u_src, vUV + dir * push * px).rgb, mn, mx);
	// Gradient-reversal sharpen: subtract a fraction of the Laplacian,
	// clamped to the 3x3 min/max so no halos form.
	float lap = (tc + ml + mr + bc) * 0.25 - mc;
	c = clamp(c - lap * 0.35 * clamp(g * 3.0, 0.0, 1.0), mn, mx);
	out_color = vec4(c, 1.0);
}
)UP4KP";

}  // namespace

const char* VideoUpscaler::mode_name(UpscaleMode m) {
	switch (m) {
	case UpscaleMode::Fsr1: return "FSR1";
	case UpscaleMode::Fsr1Sharp: return "FSR1+RCAS";
	case UpscaleMode::Anime4k: return "Anime4K";
	case UpscaleMode::CinemaHq: return "CinemaHQ";
	default: return "";
	}
}

bool VideoUpscaler::gl_ok(const char* what) {
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
		if (!logged_fail_) {
			logged_fail_ = true;
			dlog("upscale: %s: GL error 0x%x; upscaler disabled", what, unsigned(e));
		}
		return false;
	}
	return true;
}

bool VideoUpscaler::ensure_fsr1() {
	if (easu_prog_) return true;
	auto fail = [&] {
		if (easu_prog_) glDeleteProgram(easu_prog_);
		if (rcas_prog_) glDeleteProgram(rcas_prog_);
		easu_prog_ = rcas_prog_ = 0;
		logged_fail_ = true;
		dlog("upscale: FSR1 shaders unavailable; upscaler disabled");
		return false;
	};
	easu_prog_ = hui::gfx::build_program("upscale_easu", kUpscaleVert, kUpscaleEasuFrag);
	if (!easu_prog_) return fail();
	rcas_prog_ = hui::gfx::build_program("upscale_rcas", kUpscaleVert, kUpscaleRcasFrag);
	if (!rcas_prog_) return fail();
	u_easu_src_ = glGetUniformLocation(easu_prog_, "u_src");
	u_rcas_src_ = glGetUniformLocation(rcas_prog_, "u_src");
	u_rcas_sharp_ = glGetUniformLocation(rcas_prog_, "u_sharp");
	if (u_easu_src_ < 0 || u_rcas_src_ < 0 || u_rcas_sharp_ < 0)
		return fail();
	// Anime4K is optional: a compile failure only disables Anime4K, not FSR1.
	anime4k_prog_ = hui::gfx::build_program("upscale_anime4k", kUpscaleVert, kUpscaleAnime4kFrag);
	if (anime4k_prog_) {
		u_anime4k_src_ = glGetUniformLocation(anime4k_prog_, "u_src");
		if (u_anime4k_src_ < 0) {
			glDeleteProgram(anime4k_prog_);
			anime4k_prog_ = 0;
			dlog("upscale: Anime4K uniform missing; Anime4K disabled");
		} else {
			dlog("upscale: Anime4K ready");
		}
	} else {
		dlog("upscale: Anime4K shader failed to compile; Anime4K disabled");
	}
	// Anime4K with adjustable line-push strength (u_push uniform): optional
	// like Anime4K. Used when available; otherwise the header's fixed-push
	// shader above handles Anime4K mode.
	anime4k_push_prog_ = hui::gfx::build_program("upscale_anime4k_push", kUpscaleVert, kAnime4kPushFrag);
	if (anime4k_push_prog_) {
		u_push_src_ = glGetUniformLocation(anime4k_push_prog_, "u_src");
		u_push_push_ = glGetUniformLocation(anime4k_push_prog_, "u_push");
		if (u_push_src_ < 0 || u_push_push_ < 0) {
			glDeleteProgram(anime4k_push_prog_);
			anime4k_push_prog_ = 0;
			dlog("upscale: Anime4K-push uniform missing; fixed-push Anime4K only");
		} else {
			dlog("upscale: Anime4K push-strength ready");
		}
	} else {
		dlog("upscale: Anime4K-push shader failed to compile; fixed-push Anime4K only");
	}
	// Cinema HQ (anti-ringing bicubic + RCAS): optional, like Anime4K.
	cinema_prog_ = hui::gfx::build_program("upscale_cinema", kUpscaleVert, kCinemaFrag);
	if (cinema_prog_) {
		u_cinema_src_ = glGetUniformLocation(cinema_prog_, "u_src");
		if (u_cinema_src_ < 0) {
			glDeleteProgram(cinema_prog_);
			cinema_prog_ = 0;
			dlog("upscale: Cinema HQ uniform missing; Cinema HQ disabled");
		} else {
			dlog("upscale: Cinema HQ ready");
		}
	} else {
		dlog("upscale: Cinema HQ shader failed to compile; Cinema HQ disabled");
	}
	glGenVertexArrays(1, &vao_);
	glGenFramebuffers(1, &fbo_);
	if (!gl_ok("upscale init")) return fail();
	dlog("upscale: FSR1 EASU+RCAS ready");
	return true;
}

bool VideoUpscaler::ensure() {
	if (easu_prog_) return true;
	if (logged_fail_) return false;
	return ensure_fsr1();
}

std::uint32_t VideoUpscaler::upscale(std::uint32_t src_tex, int src_w, int src_h,
                                               int dst_w, int dst_h, UpscaleMode mode, float sharpness,
                                               int& out_w, int& out_h) {
	if (!ready() || !src_tex || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
		return 0;
	if (mode == UpscaleMode::Anime4k && !anime4k_prog_ && !anime4k_push_prog_)
		return 0;  // Anime4K shaders unavailable; caller falls back
	if (mode == UpscaleMode::CinemaHq && !cinema_prog_)
		return 0;  // Cinema HQ shader unavailable; caller falls back
	if (mode != UpscaleMode::Fsr1 && mode != UpscaleMode::Fsr1Sharp &&
	    mode != UpscaleMode::Anime4k && mode != UpscaleMode::CinemaHq)
		return 0;
	// Sharpening modes still do something at 1:1 (RCAS alone, no spatial
	// pass). Pure upscalers have nothing to do at 1:1.
	const bool at_1to1 = (src_w == dst_w && src_h == dst_h);
	const bool sharpen_only = (mode == UpscaleMode::Fsr1Sharp || mode == UpscaleMode::CinemaHq);
	if (at_1to1) {
		if (!sharpen_only)
			return 0;  // nothing to upscale; caller draws directly
	} else if (!(src_w < dst_w && src_h < dst_h)) {
		return 0;  // nothing to upscale; caller draws directly
	}

	// Working textures at display res. Both are always dst_w x dst_h; the size
	// is tracked once (tmp_w_/tmp_h_) and both are (re)allocated together.
	auto ensure_tex = [&](unsigned& tex) -> bool {
		if (tex) return true;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dst_w, dst_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		tex_params();
		glBindTexture(GL_TEXTURE_2D, 0);
		if (!gl_ok("upscale tex alloc")) {
			if (tex) glDeleteTextures(1, &tex);
			tex = 0;
			return false;
		}
		return true;
	};
	if (tmp_w_ != dst_w || tmp_h_ != dst_h) {
		if (tmp_tex_) { glDeleteTextures(1, &tmp_tex_); tmp_tex_ = 0; }
		if (out_tex_) { glDeleteTextures(1, &out_tex_); out_tex_ = 0; }
		tmp_w_ = dst_w;
		tmp_h_ = dst_h;
		fbo_checked_ = false;
	}
	if (!ensure_tex(tmp_tex_)) return 0;
	// RCAS needs its own output texture (can't read+write the same one).
	if ((mode == UpscaleMode::Fsr1Sharp || mode == UpscaleMode::CinemaHq) && !ensure_tex(out_tex_)) return 0;

	GLint vp[4];
	glGetIntegerv(GL_VIEWPORT, vp);
	GLint prev_fbo = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	// hui's batch renderer leaves scissor/blend enabled; a fullscreen
	// triangle drawn under them gets clipped or composited against the
	// uninitialized working textures. Run the passes with a clean slate.
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean blend = glIsEnabled(GL_BLEND);
	GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
	glViewport(0, 0, dst_w, dst_h);

	auto draw_easu = [&]() -> bool {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp_tex_, 0);
		if (!fbo_checked_ && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			if (!logged_fail_) {
				logged_fail_ = true;
				dlog("upscale: framebuffer incomplete; upscaler disabled");
			}
			return false;
		}
		// EVO's EASU needs no CPU-side constants: it derives everything
		// from textureSize(u_src) and the vUV varying.
		glUseProgram(easu_prog_);
		glUniform1i(u_easu_src_, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_tex);
		glBindVertexArray(vao_);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindVertexArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glUseProgram(0);
		return gl_ok("upscale easu");
	};

	auto draw_rcas = [&](unsigned in_tex) -> bool {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex_, 0);
		if (!fbo_checked_ && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			if (!logged_fail_) {
				logged_fail_ = true;
				dlog("upscale: framebuffer incomplete; upscaler disabled");
			}
			return false;
		}
		glUseProgram(rcas_prog_);
		glUniform1i(u_rcas_src_, 0);
		glUniform1f(u_rcas_sharp_, sharpness);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, in_tex);
		glBindVertexArray(vao_);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindVertexArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glUseProgram(0);
		return gl_ok("upscale rcas");
	};

	auto draw_cinema = [&]() -> bool {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp_tex_, 0);
		if (!fbo_checked_ && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			if (!logged_fail_) {
				logged_fail_ = true;
				dlog("upscale: framebuffer incomplete; upscaler disabled");
			}
			return false;
		}
		glUseProgram(cinema_prog_);
		glUniform1i(u_cinema_src_, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_tex);
		glBindVertexArray(vao_);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindVertexArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glUseProgram(0);
		return gl_ok("upscale cinema");
	};

	auto draw_anime4k = [&]() -> bool {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp_tex_, 0);
		if (!fbo_checked_ && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			if (!logged_fail_) {
				logged_fail_ = true;
				dlog("upscale: framebuffer incomplete; upscaler disabled");
			}
			return false;
		}
		if (anime4k_push_prog_) {
			// Line-push strength from the sharpness slider:
			// 0.25 + 0.75 * sharpness.
			glUseProgram(anime4k_push_prog_);
			glUniform1i(u_push_src_, 0);
			glUniform1f(u_push_push_, 0.25f + 0.75f * sharpness);
		} else {
			// Fallback: the header shader with hardcoded push.
			glUseProgram(anime4k_prog_);
			glUniform1i(u_anime4k_src_, 0);
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_tex);
		glBindVertexArray(vao_);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindVertexArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glUseProgram(0);
		return gl_ok("upscale anime4k");
	};

	bool ok = false;
	GLuint result = 0;
	if (mode == UpscaleMode::Anime4k) {
		// Single-pass Anime4K: src -> tmp_tex_ at display res.
		ok = draw_anime4k();
		result = tmp_tex_;
	} else if (at_1to1) {
		// 1:1: sharpening modes run RCAS directly on the source texture
		// (no spatial pass), into the output FBO.
		ok = draw_rcas(src_tex);
		result = out_tex_;
	} else if (mode == UpscaleMode::CinemaHq) {
		// Cinema HQ: anti-ringing bicubic spatial pass, then RCAS with the
		// sharpness-slider strength.
		ok = draw_cinema();
		result = tmp_tex_;
		if (ok) {
			ok = draw_rcas(tmp_tex_);
			result = out_tex_;
		}
	} else {
		ok = draw_easu();
		result = tmp_tex_;
		if (ok && mode == UpscaleMode::Fsr1Sharp) {
			ok = draw_rcas(tmp_tex_);
			result = out_tex_;
		}
	}
	fbo_checked_ = true;
	if (scissor) glEnable(GL_SCISSOR_TEST);
	if (blend) glEnable(GL_BLEND);
	if (depth) glEnable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	if (!ok) return 0;
	out_w = dst_w;
	out_h = dst_h;
	return result;
}
