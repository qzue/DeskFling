#pragma once
// GPU cinematic black hole for the Core Animation renderers (UI thread only).
//
// One CAMetalLayer covers a square region centred on the hole. Each frame:
//  1. icon copies inside the region (not grabbed, not dissolving) are drawn
//     into an offscreen texture;
//  2. a full-region pass ray-marches bent light rays (Schwarzschild photon
//     orbit approximation, units of Rs) through a thin accretion disk with
//     Keplerian, flow-mapped turbulence, Doppler beaming, gravitational
//     redshift and a volumetric haze, and samples the icon texture through a
//     thin-lens mapping (tapered to identity at the region edge);
//  3. dissolving/grabbed copies are drawn unlensed with a cell erosion mask,
//     then up to 400 additive particles per copy spiral into the hole.
// Particles are stateless functions of (cell, seed, progress, clock): pause,
// reset and migration need no particle state. Emission is premultiplied with
// zero alpha (pure additive light); only the shadow and disk density occlude.
// The shader is compiled asynchronously on first use; until it is ready (or
// if Metal is unavailable) callers keep the cached-layer fallback.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/render/draw_list.h"

namespace dpp::mac {

namespace bhm {
struct Uniforms {
    simd_float2 center;  // hole centre, scene points (y down)
    simd_float2 origin;  // region top-left, scene points
    float extent;        // region half size, points
    float radius;        // shadow radius, points
    float time;          // seconds, wrapped at 1200
    float rsPerPoint;    // Schwarzschild radii per point
    float elevation;     // camera elevation above the disk plane, radians
    float exposure;
    float diskIn, diskOut;  // Rs
    float lensE;            // thin-lens Einstein radius, Rs
    float tilt;             // 2D screen roll angle, radians (clockwise-positive)
};
struct Instance {
    simd_float2 center, size, fit;
    float rotation, tidal, consume, outlined, seed, opacity;
};
static_assert(sizeof(Uniforms) == 56 && sizeof(Instance) == 48);
constexpr int kParticlesPerCopy = 576;  // 24 x 24 cells
constexpr std::size_t kMaxCopies = 48;
constexpr float kRegionRadii = 5.6f;    // region half size in shadow radii
constexpr float kMaxDrawable = 1600;    // pixels per side
constexpr float kMaxPixelScale = 2.0f;  // full 2x Retina sharpness for fine disk filaments

inline const char* const kShader = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Uniforms { float2 center; float2 origin; float extent; float radius; float time; float rsPerPoint;
                  float elevation; float exposure; float diskIn; float diskOut; float lensE; float tilt; };
struct Instance { float2 center; float2 size; float2 fit; float rotation; float tidal; float consume;
                  float outlined; float seed; float opacity; };

float hash12(float2 p) { float3 p3 = fract(float3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33);
                         return fract((p3.x + p3.y) * p3.z); }
float hash13(float3 p3) { p3 = fract(p3 * 0.1031); p3 += dot(p3 * 0.1031, p3.zyx + 31.32); return fract((p3.x + p3.y) * p3.z); }
float vnoise(float3 x) {
    float3 i = floor(x), f = fract(x); f = f * f * (3.0 - 2.0 * f);
    float a = hash13(i), b = hash13(i + float3(1, 0, 0)), c = hash13(i + float3(0, 1, 0)), d = hash13(i + float3(1, 1, 0));
    float e = hash13(i + float3(0, 0, 1)), g = hash13(i + float3(1, 0, 1)), h = hash13(i + float3(0, 1, 1)), k = hash13(i + float3(1, 1, 1));
    return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}
float2 rot(float2 v, float a) { float c = cos(a), s = sin(a); return float2(c * v.x - s * v.y, s * v.x + c * v.y); }
float2 toNdc(float2 scene, constant Uniforms& u) {
    float2 q = (scene - u.origin) / (2.0 * u.extent); return float2(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0); }

// NASA-style volcanic ramp: deep crimson -> scarlet-orange -> bright golden-orange -> hot core.
float3 nasaPalette(float t) {
    t = clamp(t, 0.0, 2.0);
    float3 c0 = float3(0.08, 0.002, 0.000), c1 = float3(0.66, 0.024, 0.003);
    float3 c2 = float3(0.98, 0.18, 0.008),  c3 = float3(1.00, 0.46, 0.03);
    float3 c4 = float3(1.00, 0.74, 0.22);
    if (t < 0.25) return mix(c0, c1, t / 0.25);
    if (t < 0.55) return mix(c1, c2, (t - 0.25) / 0.30);
    if (t < 0.95) return mix(c2, c3, (t - 0.55) / 0.40);
    return mix(c3, c4, clamp((t - 0.95) / 0.75, 0.0, 1.0));
}
// Crisp concentric dark/bright lanes + orbiting spiral arms + sheared Keplerian plasma wisps.
// Exact seamless wrap at u.time = 1200 s (all angular rates are integer multiples of 2*pi/1200; P = 6 divides 1200).
float diskTexture(float r, float phi, float time, float diskIn) {
    float lr = log(max(r / diskIn, 1.0001));
    float inward = lr * 14.0 + time * 0.5235988;
    float rings1 = vnoise(float3(inward, cos(phi) * 1.4, sin(phi) * 1.4 + time * 0.3141593));
    float rings2 = vnoise(float3(lr * 32.0 + time * 0.8377580, 3.1, time * 0.2094395));
    float ringLanes = smoothstep(0.18, 0.82, 0.58 * rings1 + 0.42 * rings2);

    // Continuous orbiting spiral plasma arms & glowing eddies for unmistakable live rotation
    float angFast = phi - time * 1.8849556 + lr * 3.8;
    float angMid  = phi - time * 1.1519173 + lr * 5.6;
    float armWave = 0.5 + 0.5 * sin(angFast * 2.0 + 1.4 * vnoise(float3(cos(angMid) * 2.2, sin(angMid) * 2.2, lr * 7.5)));
    float flare   = smoothstep(0.50, 0.92, vnoise(float3(cos(angFast) * 2.6, sin(angFast) * 2.6, lr * 6.0 + 4.2)));

    float omega = 2.65 * pow(diskIn / r, 1.5);
    const float P = 6.0;
    float ph = time / P, fa = fract(ph), fb = fract(ph + 0.5);
    float sa = fmod(floor(ph), 200.0), sb = fmod(floor(ph + 0.5), 200.0);
    float wa = 0.5 - 0.5 * cos(6.2831853 * fa);

    float twA = (phi - omega * fa * P) + lr * 5.2;
    float saVal = 0.65 * vnoise(float3(cos(twA) * 2.8, sin(twA) * 2.8, lr * 15.0) + sa * 9.13) +
                  0.35 * vnoise(float3(cos(twA * 2.0) * 3.4, sin(twA * 2.0) * 3.4, lr * 32.0) + sa * 4.71);
    float twB = (phi - omega * fb * P) + lr * 5.2;
    float sbVal = 0.65 * vnoise(float3(cos(twB) * 2.8, sin(twB) * 2.8, lr * 15.0) + sb * 9.13) +
                  0.35 * vnoise(float3(cos(twB * 2.0) * 3.4, sin(twB * 2.0) * 3.4, lr * 32.0) + sb * 4.71);
    // Variance-preserving blend keeps spiral filament contrast crisp across phase transitions.
    float norm = rsqrt(wa * wa + (1.0 - wa) * (1.0 - wa));
    float streaks = smoothstep(0.14, 0.86, 0.5 + ((wa * saVal + (1.0 - wa) * sbVal) - 0.5) * norm);
    float combined = ringLanes * (0.28 + 0.72 * streaks) * (0.52 + 0.78 * armWave) + 0.44 * flare * ringLanes;
    return clamp(combined, 0.0, 1.55);
}
// Evaluates thin equatorial disk crossing. `rayBlock` attenuates internal ray transmittance (including
// the near-side cool outer rim that casts the clean horizontal dark gap above the lower lensed arc);
// `bgBlock` only occludes desktop/icon background where the disk visibly glows.
float3 evalDisk(float3 hit, float3 dir, constant Uniforms& u, int order, thread float& rayBlock, thread float& bgBlock) {
    float r = length(hit.xz);
    float glowOut = u.diskOut * 0.84;
    if (order > 0) {
        // True relativistic n>=1 secondary/photon-ring crossing with live orbiting plasma shimmer.
        float bell = exp(-pow(log(max(r, 0.5) / (u.diskIn * 1.65)) / 0.78, 2.0));
        bell *= smoothstep(u.diskIn * 0.62, u.diskIn * 0.88, r) * (1.0 - smoothstep(u.diskOut * 1.4, u.diskOut * 2.4, r));
        float phiL = atan2(hit.z, hit.x) - u.time * 2.1991149 + log(max(r, 0.5)) * 2.6;
        float lensShimmer = 0.66 + 0.68 * vnoise(float3(cos(phiL) * 2.4, sin(phiL) * 2.4, log(max(r, 0.5)) * 8.0));
        float3 vdir = normalize(float3(-hit.z, 0.0, hit.x));
        float v = min(sqrt(0.5 / max(r - 1.0, 0.8)), 0.50);
        float dop = sqrt(1.0 - v * v) / (1.0 - v * dot(vdir, -dir));
        float g = clamp(dop * sqrt(max(1.0 - 1.0 / max(r, 1.5), 0.15)), 0.55, 1.45);
        float scale = (order == 1 ? 0.95 : 0.45) * lensShimmer;
        rayBlock = 0.65 * bell;
        bgBlock = 0.0;
        return nasaPalette(0.58 * g * (0.85 + 0.25 * lensShimmer)) * bell * pow(g, 1.6) * scale;
    }
    float x = (r - u.diskIn) / (glowOut - u.diskIn);
    if (x <= 0.0 || r >= u.diskOut * 1.18) { rayBlock = 0.0; bgBlock = 0.0; return float3(0.0); }

    float inner = smoothstep(0.0, 0.032, x);
    float glowEnv = x < 1.0 ? inner * pow(clamp(1.0 - smoothstep(0.08, 1.0, x), 0.0, 1.0), 1.35) : 0.0;
    float rimSpan = (r - u.diskIn) / (u.diskOut * 1.18 - u.diskIn);
    float rimEnv = inner * pow(clamp(1.0 - smoothstep(0.58, 1.0, rimSpan), 0.0, 1.0), 1.4);

    float phi = atan2(hit.z, hit.x);
    float tex = diskTexture(r, phi, u.time, u.diskIn);
    float3 vdir = normalize(float3(-hit.z, 0.0, hit.x));
    float v = min(sqrt(0.5 / max(r - 1.0, 0.8)), 0.44);
    float dop = sqrt(1.0 - v * v) / (1.0 - v * dot(vdir, -dir));
    float g = dop * sqrt(max(1.0 - 1.0 / r, 0.12));

    float radialHeat = pow(u.diskIn / r, 0.68);
    float gTemp = mix(1.0, clamp(g, 0.62, 1.42), 0.68);
    float temp = (0.36 + 0.52 * radialHeat) * gTemp * (0.76 + 0.36 * tex);
    float boost = clamp(pow(g, 2.15), 0.38, 2.65);
    float intensity = pow(u.diskIn / r, 1.05) * glowEnv * (0.22 + 1.65 * tex) * boost * 2.55;

    float graze = clamp(0.15 / max(abs(dir.y), 0.03), 1.0, 3.2);
    float edgeOn = smoothstep(0.32, 0.05, abs(dir.y));
    float baseSolid = clamp(glowEnv * (0.62 + 0.38 * tex), 0.0, 0.96);
    float rimSolid = hit.z < 0.0 ? mix(baseSolid, clamp(rimEnv * 0.995, 0.0, 0.998), edgeOn) : baseSolid;
    rayBlock = 1.0 - pow(1.0 - rimSolid, graze);
    bgBlock  = 1.0 - pow(1.0 - clamp(glowEnv * (0.58 + 0.42 * tex), 0.0, 0.95), graze * 0.85);
    return nasaPalette(temp) * intensity;
}

struct HoleOut { float4 pos [[position]]; float2 p; };
vertex HoleOut hole_vs(uint vid [[vertex_id]], constant Uniforms& u [[buffer(0)]]) {
    float2 ndc = float2(float((vid << 1) & 2), float(vid & 2)) * 2.0 - 1.0;
    HoleOut o; o.pos = float4(ndc, 0.0, 1.0); o.p = float2(ndc.x, -ndc.y) * u.extent; return o;
}
fragment float4 hole_fs(HoleOut in [[stage_in]], constant Uniforms& u [[buffer(0)]],
                        texture2d<float> bg [[texture(0)]], sampler smp [[sampler(0)]]) {
    float2 lp = rot(in.p, -u.tilt);
    float2 b = float2(lp.x, -lp.y) * u.rsPerPoint;              // hole-local impact parameter, Rs, y up
    float el = abs(u.elevation) < 1e-3 ? (u.elevation >= 0.0 ? 1e-3 : -1e-3) : u.elevation;
    float se = sin(el), ce = cos(el);
    float3 fwd = float3(0.0, -se, ce), right = float3(1.0, 0.0, 0.0), up = float3(0.0, ce, se);

    // Pinhole camera at D = 26 Rs (above the disk plane) + exact energy-normalised Schwarzschild null geodesic.
    const float D = 26.0;
    float3 pos = -fwd * D;
    float3 dir0 = normalize(fwd + (b.x / D) * right + (b.y / D) * up);
    float3 hv = cross(pos, dir0); float h2 = dot(hv, hv);
    float r0 = length(pos);
    float3 vel = dir0 * sqrt(1.0 + h2 / (r0 * r0 * r0));

    float T_ray = 1.0, T_bg = 1.0, holeShadow = 0.0;
    float3 E = float3(0.0);
    bool captured = false;
    int innerCrossings = 0;

    for (int i = 0; i < 240; ++i) {
        float r = length(pos);
        if (r < 1.02) { captured = true; break; }
        if (r > 28.0 && dot(pos, vel) > 0.0) break;
        float ds = clamp(0.022 * r * r, 0.008, 0.85);
        float invR2 = 1.0 / (r * r);
        float3 acc = -1.5 * h2 * pos * (invR2 * invR2 / r);
        float3 np = pos + vel * ds + 0.5 * acc * ds * ds;
        float rn = length(np);
        if (rn < 1.02) { captured = true; break; }
        float invRn2 = 1.0 / (rn * rn);
        float3 acc2 = -1.5 * h2 * np * (invRn2 * invRn2 / rn);
        float3 nvel = vel + 0.5 * (acc + acc2) * ds;

        if ((pos.y > 0.0 && np.y <= 0.0) || (pos.y < 0.0 && np.y >= 0.0)) {
            float s = (pos.y / (pos.y - np.y)) * ds;
            float fy = pos.y + vel.y * s + 0.5 * acc.y * s * s;
            float dfy = vel.y + acc.y * s;
            if (abs(dfy) > 1e-5) s = clamp(s - fy / dfy, 0.0, ds);
            float3 hit = pos + vel * s + 0.5 * acc * s * s;
            float3 hvel = normalize(vel + acc * s);
            float rh = length(hit.xz);

            int orderBefore = innerCrossings;
            if (rh < u.diskIn + 0.42) {
                float sh = 1.0 - smoothstep(u.diskIn + 0.06, u.diskIn + 0.42, rh);
                holeShadow = max(holeShadow, sh * T_bg);
            }
            if (rh < u.diskIn * 0.98) innerCrossings += 1;

            float minR = orderBefore > 0 ? u.diskIn * 0.62 : u.diskIn;
            float maxR = orderBefore > 0 ? u.diskOut * 2.4 : u.diskOut * 1.18;
            if (rh > minR && rh < maxR) {
                float rb = 0.0, bb = 0.0;
                float3 e = evalDisk(hit, hvel, u, orderBefore, rb, bb);
                E += T_ray * e;
                T_ray *= (1.0 - rb);
                T_bg  *= (1.0 - bb);
            }
        }
        pos = np;
        vel = nvel;
        if (T_ray < 0.012 && (T_bg * (1.0 - holeShadow)) < 0.012) break;
    }
    if (captured) holeShadow = max(holeShadow, T_bg);

    // Subtle pure-additive fiery corona (alpha = 0: never muddies wallpaper or blurs disk filaments).
    float yAspect = mix(1.0, 0.52, abs(ce));
    float2 bEll = float2(b.x * yAspect, b.y);
    float dGlow = max(length(bEll) - 2.8, 0.0);
    float dopSide = 0.72 - 0.28 * abs(ce) * clamp(b.x / 5.0, -1.0, 1.0);
    E += exp(-dGlow / 1.65) * 0.055 * dopSide * (1.0 - holeShadow) * float3(0.95, 0.16, 0.02);

    float edge = 1.0 - smoothstep(0.80, 0.97, max(abs(in.p.x), abs(in.p.y)) / u.extent);
    float3 xCol = E * u.exposure;
    float lum = max(max(xCol.r, xCol.g), xCol.b);
    float3 mapped = lum > 1e-4 ? xCol * ((1.0 - exp(-lum)) / lum) : float3(0.0);
    mapped = mix(mapped, 1.0 - exp(-xCol), smoothstep(1.1, 3.2, lum) * 0.42);
    float3 col = mapped * edge;

    float shadowCov = clamp(holeShadow, 0.0, 1.0) * edge;
    float visBg = T_bg * (1.0 - shadowCov);
    float4 back = float4(0.0);
    if (visBg > 1e-3) {
        // Sample background icons in un-rotated screen space so tilting the disk never rotates nearby icons.
        float2 bBg = float2(in.p.x, -in.p.y) * u.rsPerPoint;
        float blBg = length(bBg);
        float taper = 1.0 - smoothstep(0.55, 0.8, blBg / (u.extent * u.rsPerPoint));
        float2 src = bBg * (1.0 - u.lensE * u.lensE / max(blBg * blBg, 1e-3) * taper);
        float2 uv = (float2(src.x, -src.y) / u.rsPerPoint + u.extent) / (2.0 * u.extent);
        back = bg.sample(smp, uv);
    }
    float occl = clamp(1.0 - T_bg, 0.0, 1.0) * edge;
    float alpha = clamp(occl + T_bg * shadowCov + visBg * back.a, 0.0, 1.0);
    return float4(col + visBg * back.rgb, alpha);
}

struct IconOut { float4 pos [[position]]; float2 uv; };
vertex IconOut icon_vs(uint vid [[vertex_id]], constant Uniforms& u [[buffer(0)]], constant Instance& it [[buffer(1)]]) {
    float2 corner = float2(float(vid & 1), float(vid >> 1));
    float2 scene = it.center + rot((corner - 0.5) * it.size, it.rotation);
    IconOut o; o.pos = float4(toNdc(scene, u), 0.0, 1.0); o.uv = corner; return o;
}
// Cells facing the hole go first, with per-cell noise.
float cellThreshold(float2 cu, constant Instance& it, constant Uniforms& u) {
    float2 scene = it.center + rot((cu - 0.5) * it.fit * it.size, it.rotation);
    float2 toHole = u.center - it.center; float l = length(toHole);
    float facing = l > 1e-3 ? dot(normalize(scene - it.center + 1e-4), toHole / l) : 0.0;
    return 0.6 * (0.5 - 0.5 * facing) + 0.4 * hash12(floor(cu * 24.0) + it.seed * 17.0);
}
float erodeLevel(constant Instance& it) { return max(it.tidal * 0.35, it.consume / 0.55); }
fragment float4 icon_fs(IconOut in [[stage_in]], constant Uniforms& u [[buffer(0)]], constant Instance& it [[buffer(1)]],
                        texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    float2 iu = (in.uv - 0.5) / it.fit + 0.5;
    float4 c = float4(0.0);
    if (all(iu >= 0.0) && all(iu <= 1.0)) c = tex.sample(smp, iu);
    float level = erodeLevel(it);
    if (level > 0.0) {
        // Fine grain: a 48-cell noise breaks up the 24-cell particle grid edge.
        float thr = cellThreshold((floor(clamp(iu, 0.0, 0.9999) * 24.0) + 0.5) / 24.0, it, u);
        thr += (hash12(floor(iu * 48.0) + it.seed) - 0.5) * 0.06;
        if (thr < level) c = float4(0.0);
        else if (thr < level + 0.08) c.rgb = mix(c.rgb, float3(1.0, 0.5, 0.15) * c.a, 0.8 * (1.0 - (thr - level) / 0.08));
    }
    if (it.outlined > 0.5) {
        float2 px = in.uv * it.size, d2 = min(px, it.size - px);
        float a = 1.0 - clamp(level * 2.0, 0.0, 1.0);
        if (min(d2.x, d2.y) < 1.5) c = mix(c, float4(1.0, 0.58, 0.0, 1.0) * a, a);
    }
    return c * it.opacity;
}

struct PartOut { float4 pos [[position]]; float2 uv; float4 color; };
vertex PartOut particle_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant Uniforms& u [[buffer(0)]],
                           constant Instance& it [[buffer(1)]], texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
    PartOut o; o.pos = float4(2.0, 2.0, 0.0, 1.0); o.uv = float2(0.0); o.color = float4(0.0);  // culled
    float2 cell = float2(float(iid % 24u), float(iid / 24u));
    float2 cu = (cell + 0.5) / 24.0;
    float4 ic = tex.sample(smp, cu, level(0.0));
    if (ic.a < 0.15) return o;
    float thr = cellThreshold(cu, it, u), e;
    if (it.consume > 0.0) {                       // swallowed: every cell is released once
        float tr = thr * 0.55;
        if (it.consume <= tr) return o;
        e = clamp((it.consume - tr) / 0.45, 0.0, 1.0);
    } else {                                      // approaching: eroded cells stream in a loop
        if (thr >= it.tidal * 0.35) return o;
        e = fract(u.time / 1.4 + hash12(cell * 1.7 + it.seed));
    }
    if (e >= 1.0) return o;
    float2 d0 = it.center + rot((cu - 0.5) * it.fit * it.size, it.rotation) - u.center;
    float2 ld0 = rot(d0, -u.tilt);
    float jitter = hash12(cell + 3.1) - 0.5;
    float spinSign = u.elevation >= 0.0 ? -1.0 : 1.0;
    float2 lv = rot(ld0, spinSign * (e * e * 2.6 + e * 0.7) * (1.0 + 0.3 * jitter)) * pow(1.0 - e, 1.35);
    float squash = max(abs(sin(u.elevation)), 0.16);
    lv.y *= mix(1.0, squash, smoothstep(0.0, 0.85, e));  // settle into the angled disk plane
    float a = ic.a * smoothstep(0.0, 0.06, e) * (1.0 - smoothstep(0.7, 1.0, e));
    float farSide = u.elevation >= 0.0 ? -lv.y : lv.y;
    if (farSide > 0.0)
        a *= mix(smoothstep(u.radius * 1.05, u.radius * 1.32, length(lv)), 1.0,
                 smoothstep(0.55, 0.9, abs(sin(u.elevation))));
    float2 v = rot(lv, u.tilt);
    float3 col = mix(ic.rgb / max(ic.a, 1e-3), float3(1.0, 0.5, 0.18), smoothstep(0.1, 0.6, e)) * (1.0 + 1.2 * e);
    float size = mix(1.5, 0.55, e) * (0.7 + 0.6 * hash12(cell + 9.2)) * clamp(it.size.x / 48.0, 0.6, 2.0);
    float2 corner = float2(float(vid & 1), float(vid >> 1));
    o.pos = float4(toNdc(u.center + v + (corner - 0.5) * 2.0 * size, u), 0.0, 1.0);
    o.uv = corner; o.color = float4(col * a, 0.0);
    return o;
}
fragment float4 particle_fs(PartOut in [[stage_in]]) {
    float2 q = in.uv * 2.0 - 1.0; return in.color * exp(-dot(q, q) * 2.5);
}
)METAL";

// Shared with the async compile completion; never touched off-thread after `ready`.
struct Pipelines {
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> hole = nil, icon = nil, particle = nil;
    id<MTLSamplerState> sampler = nil;
    bool ready = false, failed = false;
    NSString* error = nil;
};
}  // namespace bhm

class BlackHoleMetal {
   public:
    enum class Claim { None, Background, Foreground };

    // Starts the async shader compile once. Returns true when this frame can use Metal.
    bool begin(const BlackHoleItem& hole, CALayer* root, CGFloat scale, bool outlined) {
        bg_.clear();
        fg_.clear();
        active_ = false;
        if (!startCompile() || !state_->ready || !std::isfinite(hole.radius) || hole.radius <= 0)
            return false;
        const float extent = hole.radius * bhm::kRegionRadii;
        const CGFloat px =
            std::min<CGFloat>(std::min<CGFloat>(std::max<CGFloat>(scale, 1), bhm::kMaxPixelScale),
                              bhm::kMaxDrawable / (2 * extent));
        if (!layer_) {
            layer_ = [CAMetalLayer layer];
            layer_.device = state_->device;
            layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer_.framebufferOnly = YES;
            layer_.opaque = NO;
            layer_.presentsWithTransaction = YES;  // in step with the CA icon layers we hide
            layer_.maximumDrawableCount = 3;
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            layer_.colorspace = space;
            CGColorSpaceRelease(space);
        }
        if (layer_.superlayer != root) [root addSublayer:layer_];
        layer_.hidden = NO;
        layer_.contentsScale = px;
        layer_.frame = CGRectMake(hole.center.x - extent, hole.center.y - extent, 2 * extent, 2 * extent);
        const CGSize drawable = CGSizeMake(std::round(2 * extent * px), std::round(2 * extent * px));
        if (!CGSizeEqualToSize(layer_.drawableSize, drawable)) layer_.drawableSize = drawable;
        drawable_ = [layer_ nextDrawable];
        if (!drawable_) return false;
        uniforms_ = {};
        uniforms_.center = simd_make_float2(hole.center.x, hole.center.y);
        uniforms_.origin = simd_make_float2(hole.center.x - extent, hole.center.y - extent);
        uniforms_.extent = extent;
        uniforms_.radius = hole.radius;
        uniforms_.time = std::isfinite(hole.time) ? hole.time : 0;
        uniforms_.rsPerPoint = 2.598f / hole.radius;  // critical impact parameter = shadow radius
        uniforms_.elevation =
            std::isfinite(hole.elevation) ? std::clamp(hole.elevation, -1.4835298f, 1.4835298f) : .056f;
        uniforms_.exposure = 1.25f;
        uniforms_.diskIn = 3.0f;  // Schwarzschild ISCO = 3 Rs (6 GM/c^2)
        uniforms_.diskOut = 13.2f;
        uniforms_.lensE = 3.0f;
        uniforms_.tilt = std::isfinite(hole.tilt) ? hole.tilt : 0.0f;
        outlined_ = outlined;
        active_ = true;
        return true;
    }
    void setZ(CGFloat z) {
        if (layer_) layer_.zPosition = z;
    }
    // Called for each sprite while `begin` is active; Claim != None hides the CA layer.
    Claim claim(const SpriteItem& s, std::uint32_t textureIndex, CGImageRef image) {
        if (!active_ || !image || !s.center.isFinite() || !s.size.isFinite()) return Claim::None;
        const Vec2 c{uniforms_.center.x, uniforms_.center.y};
        Claim kind = Claim::None;
        if (s.consume > 0)
            kind = Claim::Foreground;
        else if (s.grabbed)
            kind = s.tidal > 0 ? Claim::Foreground : Claim::None;
        else if ((s.center - c).length() + .5f * s.size.length() < uniforms_.extent * .97f)
            kind = Claim::Background;
        if (kind == Claim::None || bg_.size() + fg_.size() >= bhm::kMaxCopies) return Claim::None;
        if (s.consume >= 1 || s.opacity <= 0) return kind;  // fully swallowed: hidden, nothing to draw
        id<MTLTexture> texture = textureFor(textureIndex, image);
        if (!texture) return Claim::None;
        const float imageAspect = static_cast<float>(texture.width) / std::max<float>(1, texture.height);
        const float sizeAspect = s.size.x / std::max(1e-3f, s.size.y);
        bhm::Instance it{};
        it.center = simd_make_float2(s.center.x, s.center.y);
        it.size = simd_make_float2(s.size.x, s.size.y);
        it.fit = imageAspect > sizeAspect ? simd_make_float2(1, sizeAspect / imageAspect)
                                          : simd_make_float2(imageAspect / sizeAspect, 1);
        it.rotation = std::isfinite(s.rotation) ? s.rotation : 0;
        it.tidal = std::clamp(s.tidal, 0.0f, 1.0f);
        it.consume = std::clamp(s.consume, 0.0f, 1.0f);
        it.outlined = outlined_ ? 1 : 0;
        it.seed = static_cast<float>(textureIndex % 977);
        it.opacity = std::clamp(s.opacity, 0.0f, 1.0f);
        (kind == Claim::Background ? bg_ : fg_).push_back({it, texture});
        return kind;
    }
    void finish() {
        if (!active_) return hide();
        active_ = false;
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        const auto size = layer_.drawableSize;
        if (!bgTexture_ || bgTexture_.width != size.width || bgTexture_.height != size.height) {
            MTLTextureDescriptor* d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:static_cast<NSUInteger>(size.width)
                                                                  height:static_cast<NSUInteger>(size.height)
                                                               mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            bgTexture_ = [state_->device newTextureWithDescriptor:d];
        }
        // 1. Lensable icon copies.
        MTLRenderPassDescriptor* bgPass = [MTLRenderPassDescriptor renderPassDescriptor];
        bgPass.colorAttachments[0].texture = bgTexture_;
        bgPass.colorAttachments[0].loadAction = MTLLoadActionClear;
        bgPass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        bgPass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:bgPass];
        drawIcons(enc, bg_);
        [enc endEncoding];
        // 2. Ray-marched hole + lensed copies, 3. dissolving copies and particles.
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable_.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cb renderCommandEncoderWithDescriptor:pass];
        [enc setRenderPipelineState:state_->hole];
        [enc setVertexBytes:&uniforms_ length:sizeof(uniforms_) atIndex:0];
        [enc setFragmentBytes:&uniforms_ length:sizeof(uniforms_) atIndex:0];
        [enc setFragmentTexture:bgTexture_ atIndex:0];
        [enc setFragmentSamplerState:state_->sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        drawIcons(enc, fg_);
        [enc setRenderPipelineState:state_->particle];
        for (const auto* list : {&bg_, &fg_})
            for (const auto& copy : *list) {
                if (copy.instance.tidal <= 0 && copy.instance.consume <= 0) continue;
                [enc setVertexBytes:&copy.instance length:sizeof(copy.instance) atIndex:1];
                [enc setVertexTexture:copy.texture atIndex:0];
                [enc setVertexSamplerState:state_->sampler atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                        vertexStart:0
                        vertexCount:4
                      instanceCount:bhm::kParticlesPerCopy];
            }
        [enc endEncoding];
        [cb commit];
        [cb waitUntilScheduled];
        [drawable_ present];  // presentsWithTransaction: lands with the enclosing CATransaction
        drawable_ = nil;
    }
    void hide() {
        active_ = false;
        drawable_ = nil;
        if (layer_) layer_.hidden = YES;
    }
    // Diagnostics: nil until compiled; describes compile failure if any.
    NSString* status() const {
        if (!state_) return @"not_started";
        if (state_->failed) return state_->error ? state_->error : @"failed";
        return state_->ready ? @"ready" : @"compiling";
    }
    static constexpr std::size_t maxCopies() { return bhm::kMaxCopies; }

   private:
    struct Copy {
        bhm::Instance instance;
        id<MTLTexture> texture;
    };
    std::shared_ptr<bhm::Pipelines> state_;
    id<MTLCommandQueue> queue_ = nil;
    CAMetalLayer* layer_ = nil;
    id<CAMetalDrawable> drawable_ = nil;
    id<MTLTexture> bgTexture_ = nil;
    std::unordered_map<std::uint32_t, id<MTLTexture>> textures_;
    std::vector<Copy> bg_, fg_;
    bhm::Uniforms uniforms_{};
    bool active_ = false, outlined_ = false;

    bool startCompile() {
        if (state_) return !state_->failed;
        state_ = std::make_shared<bhm::Pipelines>();
        state_->device = MTLCreateSystemDefaultDevice();
        if (!state_->device) {
            state_->failed = true;
            state_->error = @"no_metal_device";
            return false;
        }
        queue_ = [state_->device newCommandQueue];
        std::weak_ptr<bhm::Pipelines> weak = state_;
        id<MTLDevice> device = state_->device;
        [device newLibraryWithSource:[NSString stringWithUTF8String:bhm::kShader]
                             options:[MTLCompileOptions new]
                   completionHandler:^(id<MTLLibrary> library, NSError* error) {
                     // Build pipelines off the UI thread, publish on it.
                     NSError* failure = error;
                     id<MTLRenderPipelineState> hole = nil, icon = nil, particle = nil;
                     if (library) {
                         hole = pipeline(device, library, @"hole_vs", @"hole_fs", false, &failure);
                         icon = pipeline(device, library, @"icon_vs", @"icon_fs", true, &failure);
                         particle = pipeline(device, library, @"particle_vs", @"particle_fs", true, &failure);
                     }
                     MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
                     sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
                     sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToZero;
                     id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
                     dispatch_async(dispatch_get_main_queue(), ^{
                       auto s = weak.lock();
                       if (!s) return;
                       if (hole && icon && particle && sampler) {
                           s->hole = hole, s->icon = icon, s->particle = particle, s->sampler = sampler;
                           s->ready = true;
                       } else {
                           s->failed = true;
                           s->error = failure ? failure.localizedDescription : @"pipeline_failed";
                           NSLog(@"DPP black hole Metal fallback: %@", s->error);
                       }
                     });
                   }];
        return true;
    }
    static id<MTLRenderPipelineState> pipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString* vs,
                                               NSString* fs, bool blend, NSError** error) {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library newFunctionWithName:vs];
        d.fragmentFunction = [library newFunctionWithName:fs];
        if (!d.vertexFunction || !d.fragmentFunction) return nil;
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (blend) {  // premultiplied "over"; zero-alpha output is pure additive light
            auto* a = d.colorAttachments[0];
            a.blendingEnabled = YES;
            a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = MTLBlendFactorOne;
            a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError* local = nil;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&local];
        if (!state && error && !*error) *error = local;
        return state;
    }
    void drawIcons(id<MTLRenderCommandEncoder> enc, const std::vector<Copy>& copies) {
        if (copies.empty()) return;
        [enc setRenderPipelineState:state_->icon];
        [enc setVertexBytes:&uniforms_ length:sizeof(uniforms_) atIndex:0];
        [enc setFragmentBytes:&uniforms_ length:sizeof(uniforms_) atIndex:0];
        [enc setFragmentSamplerState:state_->sampler atIndex:0];
        for (const auto& copy : copies) {
            [enc setVertexBytes:&copy.instance length:sizeof(copy.instance) atIndex:1];
            [enc setFragmentBytes:&copy.instance length:sizeof(copy.instance) atIndex:1];
            [enc setFragmentTexture:copy.texture atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
    }
    // Decoded once per texture (<=128 px, premultiplied RGBA); cached for the renderer's lifetime.
    id<MTLTexture> textureFor(std::uint32_t index, CGImageRef image) {
        if (auto it = textures_.find(index); it != textures_.end()) return it->second;
        const size_t w0 = CGImageGetWidth(image), h0 = CGImageGetHeight(image);
        if (!w0 || !h0) return nil;
        const double k = std::min(1.0, 128.0 / static_cast<double>(std::max(w0, h0)));
        const size_t w = std::max<size_t>(1, static_cast<size_t>(std::lround(w0 * k)));
        const size_t h = std::max<size_t>(1, static_cast<size_t>(std::lround(h0 * k)));
        std::vector<std::uint8_t> pixels(w * h * 4);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, space,
                                                 static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
        CGColorSpaceRelease(space);
        if (!ctx) return nil;
        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
        CGContextRelease(ctx);
        MTLTextureDescriptor* d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [state_->device newTextureWithDescriptor:d];
        [texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                   mipmapLevel:0
                     withBytes:pixels.data()
                   bytesPerRow:w * 4];
        textures_[index] = texture;
        return texture;
    }
};

}  // namespace dpp::mac
