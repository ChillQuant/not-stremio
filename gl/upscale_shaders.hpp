// Video upscaler shaders: FSR1 EASU + RCAS.
//
// Clean-room reimplementation by EVO Player (sainsaji/evo-player-ps5),
// GPL-3.0-or-later. Adapted from their AGC pipeline shaders to OpenGL:
//   - Vulkan `layout(set=N, binding=M)` replaced with plain uniforms
//   - `gl_VertexIndex` replaced with OpenGL `gl_VertexID`
//   - Vertex shader simplified (full-frame upscale needs no UV sub-rect)
//   - RCAS gains a `u_sharp` uniform for our adjustable sharpening slider
//     (EVO uses a fixed factor; u_sharp=1.0 reproduces their behavior)
//
// Source:
//   https://github.com/sainsaji/evo-player-ps5/blob/HEAD/projects/evoplayer/shaders/agc/upscale_easu.pipe
//   https://github.com/sainsaji/evo-player-ps5/blob/HEAD/projects/evoplayer/shaders/agc/upscale_rcas.pipe
//
#pragma once

// ==================== Fullscreen triangle ====================
static const char* kUpscaleVert = R"UPVERT(
layout(location = 0) out vec2 vUV;
void main() {
	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
	vUV = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)UPVERT";

// ==================== FSR1 EASU ====================
// Edge-adaptive spatial upsampling. Input: source texture at native res.
// Output: display-sized framebuffer. vUV spans 0..1 across the output,
// which maps 1:1 onto the source (full-frame upscale).
static const char* kUpscaleEasuFrag = R"UPEASU(
uniform sampler2D u_src;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

ivec2 g_sz;

vec3 S(ivec2 p) {
    return texelFetch(u_src, clamp(p, ivec2(0), g_sz - 1), 0).rgb;
}

/* FSR's cheap luma: B/2 + R/2 + G. */
float L(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

/* Direction and length contribution of one of the four bilinear corners.
 * a = up, b = left, c = centre, d = right, e = down. */
void easu_set(inout vec2 dir, inout float len, float w,
              float a, float b, float c, float d, float e) {
    float dc = d - c;
    float cb = c - b;
    float lenX = max(abs(dc), abs(cb));
    lenX = 1.0 / max(lenX, 1.0 / 65536.0);
    float dirX = d - b;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    len += lenX * lenX * w;

    float ec = e - c;
    float ca = c - a;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0 / max(lenY, 1.0 / 65536.0);
    float dirY = e - a;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    len += lenY * lenY * w;
}

/* Lanczos-2 approximation, stretched along the edge direction. */
void easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,
              float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y,
                  off.x * -dir.y + off.y * dir.x) * len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main() {
    g_sz = textureSize(u_src, 0);
    vec2 pp = vUV * vec2(g_sz) - vec2(0.5);
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 o = ivec2(fp);

    /*      b c
     *    e f g h
     *    i j k l
     *      n o      f = o */
    vec3 bC = S(o + ivec2(0, -1)), cC = S(o + ivec2(1, -1));
    vec3 eC = S(o + ivec2(-1, 0)), fC = S(o),                gC = S(o + ivec2(1, 0)), hC = S(o + ivec2(2, 0));
    vec3 iC = S(o + ivec2(-1, 1)), jC = S(o + ivec2(0, 1)), kC = S(o + ivec2(1, 1)), lC = S(o + ivec2(2, 1));
    vec3 nC = S(o + ivec2(0, 2)),  oC = S(o + ivec2(1, 2));

    float bL = L(bC), cL = L(cC), eL = L(eC), fL = L(fC), gL = L(gC), hL = L(hC);
    float iL = L(iC), jL = L(jC), kL = L(kC), lL = L(lC), nL = L(nC), oL = L(oC);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easu_set(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);
    easu_set(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);
    easu_set(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);

    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);

    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easu_tap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    easu_tap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    easu_tap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, iC);
    easu_tap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, jC);
    easu_tap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, fC);
    easu_tap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, eC);
    easu_tap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, kC);
    easu_tap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, lC);
    easu_tap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, hC);
    easu_tap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, gC);
    easu_tap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, oC);
    easu_tap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, nC);

    /* De-ring: clamp to the 2x2 neighbourhood. */
    vec3 mn = min(min(fC, gC), min(jC, kC));
    vec3 mx = max(max(fC, gC), max(jC, kC));
    vec3 pix = clamp(aC / max(aW, 1.0 / 65536.0), mn, mx);
    out_color = vec4(pix, 1.0);
}
)UPEASU";

// ==================== FSR1 RCAS ====================
// Robust contrast-adaptive sharpening. Input: EASU output (display-sized).
// Output: sharpened display-sized framebuffer. u_sharp 0.0-1.0 scales the
// sharpening lobe (1.0 = EVO's fixed strength).
static const char* kUpscaleRcasFrag = R"UPRCAS(
uniform sampler2D u_src;
uniform float u_sharp;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

ivec2 g_sz;

vec3 S(ivec2 p) {
    return texelFetch(u_src, clamp(p, ivec2(0), g_sz - 1), 0).rgb;
}

float L(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void main() {
    g_sz = textureSize(u_src, 0);
    ivec2 ip = clamp(ivec2(vUV * vec2(g_sz)), ivec2(0), g_sz - 1);

    /*   b
     * d e f
     *   h   */
    vec3 b = S(ip + ivec2(0, -1));
    vec3 d = S(ip + ivec2(-1, 0));
    vec3 e = S(ip);
    vec3 f = S(ip + ivec2(1, 0));
    vec3 h = S(ip + ivec2(0, 1));

    float bL = L(b), dL = L(d), eL = L(e), fL = L(f), hL = L(h);

    /* Noise & compression-artifact guard: back off on isolated single-pixel grain
     * and ultra-low-contrast macroblock chatter (< 2.5% local luma contrast). */
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float lmax = max(max(max(bL, dL), max(eL, fL)), hL);
    float lmin = min(min(min(bL, dL), min(eL, fL)), hL);
    float contrast = lmax - lmin;
    nz = clamp(abs(nz) / max(contrast, 1.0 / 65536.0), 0.0, 1.0);
    nz = (-0.65 * nz + 1.0) * smoothstep(0.008, 0.032, contrast);

    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0),
                     min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * (0.8705506 * u_sharp);
    lobe *= nz;

    vec3 pix = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    out_color = vec4(clamp(pix, 0.0, 1.0), 1.0);
}
)UPRCAS";

// ==================== Anime4K (single-pass) ====================
// Line-aware upscaling for anime/cartoon content. Detects luminance edges
// with a 3x3 kernel, pushes the sample point along the detected line
// direction (thinning the line), then applies a mild gradient-reversal
// sharpen. One pass, no CNN weights: captures Anime4K's characteristic
// clean-line look at a fraction of the cost.
static const char* kUpscaleAnime4kFrag = R"UP4K(
uniform sampler2D u_src;
layout(location = 0) in vec2 vUV;
out vec4 out_color;

float luma(vec3 c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

void main() {
	vec2 px = 1.0 / vec2(textureSize(u_src, 0));
	// 3x3 luma neighborhood.
	float tl = luma(texture(u_src, vUV + vec2(-px.x, -px.y)).rgb);
	float tc = luma(texture(u_src, vUV + vec2(0.0, -px.y)).rgb);
	float tr = luma(texture(u_src, vUV + vec2(px.x, -px.y)).rgb);
	float ml = luma(texture(u_src, vUV + vec2(-px.x, 0.0)).rgb);
	float mc = luma(texture(u_src, vUV).rgb);
	float mr = luma(texture(u_src, vUV + vec2(px.x, 0.0)).rgb);
	float bl = luma(texture(u_src, vUV + vec2(-px.x, px.y)).rgb);
	float bc = luma(texture(u_src, vUV + vec2(0.0, px.y)).rgb);
	float br = luma(texture(u_src, vUV + vec2(px.x, px.y)).rgb);
	// Sobel gradients -> dominant line direction.
	float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
	float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
	float g = length(vec2(gx, gy));
	// Push the sample point perpendicular to the gradient (along the line)
	// proportional to edge strength: thins lines without ringing.
	vec2 dir = g > 1e-4 ? normalize(vec2(-gy, gx)) : vec2(0.0);
	float push = clamp(g * 2.0, 0.0, 0.45);
	vec3 c = texture(u_src, vUV + dir * push * px).rgb;
	// Gradient-reversal sharpen: subtract a fraction of the Laplacian,
	// clamped to the 3x3 min/max so no halos form.
	float lap = (tc + ml + mr + bc) * 0.25 - mc;
	vec3 mn = min(min(vec3(tl), vec3(tc)), min(vec3(ml), vec3(mc)));
	vec3 mx = max(max(vec3(tl), vec3(tc)), max(vec3(ml), vec3(mc)));
	mn = min(mn, min(min(vec3(mr), vec3(bl)), min(vec3(bc), vec3(br))));
	mx = max(mx, max(max(vec3(mr), vec3(bl)), max(vec3(bc), vec3(br))));
	c = clamp(c - lap * 0.35 * clamp(g * 3.0, 0.0, 1.0), mn, mx);
	out_color = vec4(c, 1.0);
}
)UP4K";
