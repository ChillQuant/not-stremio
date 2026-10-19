// The video picture's YUV->RGB conversion on the GPU: the decoded planes go
// up as textures and a fragment shader does the YUV->RGB matrix plus the
// HDR10 tone mapping (the same math as src/yuv_convert.cpp, in float).
//
// Fail-closed by design, and never required for correct playback:
//   - ensure() returns false when anything is unsupported (shaders, texture
//     formats); the caller then keeps the CPU path for the whole stream.
//   - render() returns false on any GL failure; the caller converts the same
//     planes on the CPU instead (yuv_convert.h).
#pragma once

#include <cstdint>

#include "player.h"  // Player::Picture

class GpuYuv {
public:
	GpuYuv() = default;
	// No GL calls: the app exits through a noreturn quit(), so this never
	// runs with a dead context; while running, the objects are tiny and the
	// textures are recycled on size changes.
	~GpuYuv() = default;
	GpuYuv(const GpuYuv&) = delete;
	GpuYuv& operator=(const GpuYuv&) = delete;

	// Compile the shaders and build the PQ LUT. Must be called with the GL
	// context current (the UI thread). False when the GPU path can't be
	// used; logs once.
	bool ensure();
	bool ready() const { return program_ != 0; }

	// Upload the picture's planes and render YUV->RGB into target, an RGBA
	// texture the caller owns (recreated when the size changes; tw/th track
	// it). False on any GL failure: convert on the CPU instead.
	bool render(const Player::Picture& pic, std::uint32_t& target, int& tw, int& th);

	// Deep Blacks (0=Off, 1=Low, 2=High) and tone mapper (0=Hable, 1=BT.2390).
	// hq_chroma: the upscaler is engaged (or Deep Blacks runs), so keep the
	// bilateral 4:4:4 chroma + debanding path even on 8-bit SDR sources.
	void set_quality(int deep_blacks, int tonemap, bool hq_chroma = false) {
		deep_blacks_ = deep_blacks;
		tonemap_ = tonemap;
		hq_chroma_ = hq_chroma;
	}

private:
	bool upload_planes(const Player::Picture& pic);
	// Drain glGetError, logging the first failure site.
	bool gl_ok(const char* what);

	unsigned program_ = 0, vao_ = 0, fbo_ = 0;
	unsigned tex_y_ = 0, tex_c_ = 0, tex_v_ = 0, tex_pq_ = 0;
	int u_y_tex_ = -1, u_c_tex_ = -1, u_v_tex_ = -1, u_pq_tex_ = -1;
	int u_colors_ = -1, u_interleaved_ = -1, u_peak_ = -1, u_shift_div_ = -1;
	int u_deep_blacks_ = -1, u_tonemap_ = -1, u_src_size_ = -1, u_wide_ = -1;
	int u_fast_ = -1;
	int deep_blacks_ = 0, tonemap_ = 0;
	bool hq_chroma_ = false;
	int py_w_ = 0, py_h_ = 0, pc_w_ = 0, pc_h_ = 0;  // plane texture sizes
	bool wide_ = false, interleaved_ = false;        // plane texture formats
	bool logged_fail_ = false;
	bool fbo_checked_ = false;  // framebuffer completeness only changes when the target texture is (re)allocated
	int frames_since_alloc_ = 0;  // glGetError only checked for first 60 frames
};
