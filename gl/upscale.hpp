// Video upscaling: FSR1 (EASU+RCAS) and Anime4K for sub-display-resolution
// content. Replaces the bilinear stretch with edge-aware upscaling.
//
// Fail-closed by design, never required for correct playback:
//   - ensure() returns false when anything is unsupported; the caller keeps
//     the bilinear path.
//   - upscale() returns false on any GL failure; the caller draws the
//     source texture directly (bilinear) instead.
#pragma once

#include <cstdint>

enum class UpscaleMode {
	Off = 0,
	Fsr1 = 1,        // EASU only
	Fsr1Sharp = 2,   // EASU + RCAS sharpening
	Anime4k = 3,     // Anime4K CNN x2 S (for anime content)
	CinemaHq = 4,    // anti-ringing bicubic + RCAS sharpening
};

class VideoUpscaler {
public:
	VideoUpscaler() = default;
	~VideoUpscaler() = default;
	VideoUpscaler(const VideoUpscaler&) = delete;
	VideoUpscaler& operator=(const VideoUpscaler&) = delete;

	// Compile the shaders. Must be called with the GL context current
	// (the UI thread). False when the upscaler can't be used; logs once.
	bool ensure();
	bool ready() const { return easu_prog_ != 0; }

	// Upscale src_tex (src_w x src_h, RGBA8) to dst_w x dst_h.
	// Returns the upscaled texture on success (owned by the upscaler;
	// valid until the next upscale() call or a size change), 0 on any GL
	// failure (caller falls back to bilinear). out_w/out_h receive the
	// result size. sharpness is 0.0-1.0, used only for Fsr1Sharp.
	std::uint32_t upscale(std::uint32_t src_tex, int src_w, int src_h,
	                      int dst_w, int dst_h, UpscaleMode mode, float sharpness,
	                      int& out_w, int& out_h);

	// Human-readable name for stats-for-nerds. Empty when off/none.
	static const char* mode_name(UpscaleMode m);

private:
	bool ensure_fsr1();
	bool gl_ok(const char* what);

	unsigned easu_prog_ = 0, rcas_prog_ = 0, anime4k_prog_ = 0, cinema_prog_ = 0;
	unsigned vao_ = 0, fbo_ = 0;
	// Working textures at display res: EASU/Cinema renders into tmp_tex_,
	// RCAS reads tmp_tex_ and writes out_tex_ (no feedback loop).
	unsigned tmp_tex_ = 0, out_tex_ = 0;
	int tmp_w_ = 0, tmp_h_ = 0;
	// Uniform locations (EASU/Cinema need only the source texture; RCAS
	// adds a sharpness scalar).
	int u_easu_src_ = -1;
	int u_rcas_src_ = -1, u_rcas_sharp_ = -1;
	int u_anime4k_src_ = -1;
	// Anime4K variant with an adjustable line-push strength (u_push), for
	// Task 1.3: upscale_shaders.hpp's Anime4K shader has a hardcoded push
	// and no uniform, so this variant lives in upscale.cpp instead.
	unsigned anime4k_push_prog_ = 0;
	int u_push_src_ = -1, u_push_push_ = -1;
	int u_cinema_src_ = -1;
	bool logged_fail_ = false;
	bool fbo_checked_ = false;
};
