#pragma once
// GPU realistic black hole for the Windows D3D11 + Direct2D renderer (UI thread only).
//
// Exact HLSL counterpart of platforms/macos/black_hole_metal.h:
//  1. nearby normal icon copies are drawn into an offscreen D3D11 texture;
//  2. a full-region pixel shader integrates energy-conserving Schwarzschild null
//     geodesics from a pinhole camera at arbitrary 3D elevation & 2D screen tilt,
//     accumulating crisp concentric + sheared Keplerian spiral disk filaments,
//     natural n>=1 non-circular bell-footed photon ring, horizontal dark gap,
//     and thin-lens icon warping;
//  3. dissolving/grabbed copies render unlensed with cellular erosion + 576
//     stateless additive particles per copy spiralling into the angled disk plane.
// The output texture is wrapped once per size as an ID2D1Bitmap1 via IDXGISurface
// (zero CPU readback) and drawn at the Front BlackHoleItem slot.

#include <d2d1_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "core/render/draw_list.h"

namespace dpp::winmirror {

namespace bhd3d {

struct Uniforms {
    float centerX, centerY;
    float originX, originY;
    float extent, radius, time, rsPerPoint;
    float elevation, exposure, diskIn, diskOut;
    float lensE, tilt, pad0, pad1;
};
struct Instance {
    float centerX, centerY, sizeX, sizeY;
    float fitX, fitY, rotation, tidal;
    float consume, outlined, seed, opacity;
};
static_assert(sizeof(Uniforms) == 64 && sizeof(Instance) == 48);

constexpr UINT kParticlesPerCopy = 576;  // 24 x 24 cells
constexpr std::size_t kMaxCopies = 48;
constexpr float kRegionRadii = 5.6f;
constexpr float kMaxDrawable = 1600.0f;
constexpr float kMaxPixelScale = 2.0f;

inline const char* const kHlsl = R"HLSL(
cbuffer UniformBuf : register(b0) {
    float2 u_center;
    float2 u_origin;
    float u_extent;
    float u_radius;
    float u_time;
    float u_rsPerPoint;
    float u_elevation;
    float u_exposure;
    float u_diskIn;
    float u_diskOut;
    float u_lensE;
    float u_tilt;
    float2 u_pad;
};
cbuffer InstanceBuf : register(b1) {
    float2 it_center;
    float2 it_size;
    float2 it_fit;
    float it_rotation;
    float it_tidal;
    float it_consume;
    float it_outlined;
    float it_seed;
    float it_opacity;
};

Texture2D<float4> tex0 : register(t0);
SamplerState smp0 : register(s0);

float fract1(float x) { return x - floor(x); }
float3 fract3(float3 x) { return x - floor(x); }
float pows(float x, float y) { return pow(max(x, 1e-6), y); }

float hash12(float2 p) {
    float3 p3 = fract3(float3(p.x, p.y, p.x) * 0.1031);
    p3 += dot(p3, float3(p3.y, p3.z, p3.x) + 33.33);
    return fract1((p3.x + p3.y) * p3.z);
}
float hash13(float3 p3) {
    p3 = fract3(p3 * 0.1031);
    p3 += dot(p3 * 0.1031, float3(p3.z, p3.y, p3.x) + 31.32);
    return fract1((p3.x + p3.y) * p3.z);
}
float vnoise(float3 x) {
    float3 i = floor(x), f = fract3(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash13(i), b = hash13(i + float3(1, 0, 0)), c = hash13(i + float3(0, 1, 0)), d = hash13(i + float3(1, 1, 0));
    float e = hash13(i + float3(0, 0, 1)), g = hash13(i + float3(1, 0, 1)), h = hash13(i + float3(0, 1, 1)), k = hash13(i + float3(1, 1, 1));
    return lerp(lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y), lerp(lerp(e, g, f.x), lerp(h, k, f.x), f.y), f.z);
}
float2 rot2(float2 v, float a) {
    float c = cos(a), s = sin(a);
    return float2(c * v.x - s * v.y, s * v.x + c * v.y);
}
float2 toNdc(float2 scene) {
    float2 q = (scene - u_origin) / (2.0 * u_extent);
    return float2(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0);
}

float3 nasaPalette(float t) {
    t = clamp(t, 0.0, 2.0);
    float3 c0 = float3(0.08, 0.002, 0.000), c1 = float3(0.66, 0.024, 0.003);
    float3 c2 = float3(0.98, 0.18, 0.008),  c3 = float3(1.00, 0.46, 0.03);
    float3 c4 = float3(1.00, 0.74, 0.22);
    if (t < 0.25) return lerp(c0, c1, t / 0.25);
    if (t < 0.55) return lerp(c1, c2, (t - 0.25) / 0.30);
    if (t < 0.95) return lerp(c2, c3, (t - 0.55) / 0.40);
    return lerp(c3, c4, clamp((t - 0.95) / 0.75, 0.0, 1.0));
}

float diskTexture(float r, float phi, float time, float diskIn) {
    float lr = log(max(r / diskIn, 1.0001));
    float inward = lr * 14.0 + time * 0.5235988;
    float rings1 = vnoise(float3(inward, cos(phi) * 1.4, sin(phi) * 1.4 + time * 0.3141593));
    float rings2 = vnoise(float3(lr * 32.0 + time * 0.8377580, 3.1, time * 0.2094395));
    float ringLanes = smoothstep(0.18, 0.82, 0.58 * rings1 + 0.42 * rings2);

    float angFast = phi - time * 1.8849556 + lr * 3.8;
    float angMid  = phi - time * 1.1519173 + lr * 5.6;
    float armWave = 0.5 + 0.5 * sin(angFast * 2.0 + 1.4 * vnoise(float3(cos(angMid) * 2.2, sin(angMid) * 2.2, lr * 7.5)));
    float flare   = smoothstep(0.50, 0.92, vnoise(float3(cos(angFast) * 2.6, sin(angFast) * 2.6, lr * 6.0 + 4.2)));

    float omega = 2.65 * pows(diskIn / r, 1.5);
    const float P = 6.0;
    float ph = time / P, fa = fract1(ph), fb = fract1(ph + 0.5);
    float sa = fmod(floor(ph), 200.0), sb = fmod(floor(ph + 0.5), 200.0);
    float wa = 0.5 - 0.5 * cos(6.2831853 * fa);

    float twA = (phi - omega * fa * P) + lr * 5.2;
    float saVal = 0.65 * vnoise(float3(cos(twA) * 2.8, sin(twA) * 2.8, lr * 15.0) + sa * 9.13) +
                  0.35 * vnoise(float3(cos(twA * 2.0) * 3.4, sin(twA * 2.0) * 3.4, lr * 32.0) + sa * 4.71);
    float twB = (phi - omega * fb * P) + lr * 5.2;
    float sbVal = 0.65 * vnoise(float3(cos(twB) * 2.8, sin(twB) * 2.8, lr * 15.0) + sb * 9.13) +
                  0.35 * vnoise(float3(cos(twB * 2.0) * 3.4, sin(twB * 2.0) * 3.4, lr * 32.0) + sb * 4.71);
    float norm = rsqrt(wa * wa + (1.0 - wa) * (1.0 - wa));
    float streaks = smoothstep(0.14, 0.86, 0.5 + ((wa * saVal + (1.0 - wa) * sbVal) - 0.5) * norm);
    float combined = ringLanes * (0.28 + 0.72 * streaks) * (0.52 + 0.78 * armWave) + 0.44 * flare * ringLanes;
    return clamp(combined, 0.0, 1.55);
}

float3 evalDisk(float3 hit, float3 dir, int order, out float rayBlock, out float bgBlock) {
    float r = length(hit.xz);
    float glowOut = u_diskOut * 0.84;
    if (order > 0) {
        float bellL = log(max(r, 0.5) / (u_diskIn * 1.65)) / 0.78;
        float bell = exp(-(bellL * bellL));
        bell *= smoothstep(u_diskIn * 0.62, u_diskIn * 0.88, r) * (1.0 - smoothstep(u_diskOut * 1.4, u_diskOut * 2.4, r));
        float phiL = atan2(hit.z, hit.x) - u_time * 2.1991149 + log(max(r, 0.5)) * 2.6;
        float lensShimmer = 0.66 + 0.68 * vnoise(float3(cos(phiL) * 2.4, sin(phiL) * 2.4, log(max(r, 0.5)) * 8.0));
        float3 vdir = normalize(float3(-hit.z, 0.0, hit.x));
        float v = min(sqrt(0.5 / max(r - 1.0, 0.8)), 0.50);
        float dop = sqrt(1.0 - v * v) / (1.0 - v * dot(vdir, -dir));
        float g = clamp(dop * sqrt(max(1.0 - 1.0 / max(r, 1.5), 0.15)), 0.55, 1.45);
        float scale = ((order == 1) ? 0.95 : 0.45) * lensShimmer;
        rayBlock = 0.65 * bell;
        bgBlock = 0.0;
        return nasaPalette(0.58 * g * (0.85 + 0.25 * lensShimmer)) * bell * pows(g, 1.6) * scale;
    }
    float x = (r - u_diskIn) / (glowOut - u_diskIn);
    if (x <= 0.0 || r >= u_diskOut * 1.18) {
        rayBlock = 0.0;
        bgBlock = 0.0;
        return float3(0.0, 0.0, 0.0);
    }
    float inner = smoothstep(0.0, 0.032, x);
    float glowEnv = (x < 1.0) ? (inner * pows(clamp(1.0 - smoothstep(0.08, 1.0, x), 0.0, 1.0), 1.35)) : 0.0;
    float rimSpan = (r - u_diskIn) / (u_diskOut * 1.18 - u_diskIn);
    float rimEnv = inner * pows(clamp(1.0 - smoothstep(0.58, 1.0, rimSpan), 0.0, 1.0), 1.4);

    float phi = atan2(hit.z, hit.x);
    float tex = diskTexture(r, phi, u_time, u_diskIn);
    float3 vdir = normalize(float3(-hit.z, 0.0, hit.x));
    float v = min(sqrt(0.5 / max(r - 1.0, 0.8)), 0.44);
    float dop = sqrt(1.0 - v * v) / (1.0 - v * dot(vdir, -dir));
    float g = dop * sqrt(max(1.0 - 1.0 / r, 0.12));

    float radialHeat = pows(u_diskIn / r, 0.68);
    float gTemp = lerp(1.0, clamp(g, 0.62, 1.42), 0.68);
    float temp = (0.36 + 0.52 * radialHeat) * gTemp * (0.76 + 0.36 * tex);
    float boost = clamp(pows(g, 2.15), 0.38, 2.65);
    float intensity = pows(u_diskIn / r, 1.05) * glowEnv * (0.22 + 1.65 * tex) * boost * 2.55;

    float graze = clamp(0.15 / max(abs(dir.y), 0.03), 1.0, 3.2);
    float edgeOn = smoothstep(0.32, 0.05, abs(dir.y));
    float baseSolid = clamp(glowEnv * (0.62 + 0.38 * tex), 0.0, 0.96);
    float rimSolid = (hit.z < 0.0) ? lerp(baseSolid, clamp(rimEnv * 0.995, 0.0, 0.998), edgeOn) : baseSolid;
    rayBlock = 1.0 - pows(1.0 - rimSolid, graze);
    bgBlock  = 1.0 - pows(1.0 - clamp(glowEnv * (0.58 + 0.42 * tex), 0.0, 0.95), graze * 0.85);
    return nasaPalette(temp) * intensity;
}

struct HoleOut {
    float4 pos : SV_Position;
    float2 p : TEXCOORD0;
};
HoleOut hole_vs(uint vid : SV_VertexID) {
    float2 ndc = float2(float((vid << 1) & 2u), float(vid & 2u)) * 2.0 - 1.0;
    HoleOut o;
    o.pos = float4(ndc, 0.0, 1.0);
    o.p = float2(ndc.x, -ndc.y) * u_extent;
    return o;
}
float4 hole_ps(HoleOut inp) : SV_Target {
    float2 lp = rot2(inp.p, -u_tilt);
    float2 b = float2(lp.x, -lp.y) * u_rsPerPoint;
    float el = (abs(u_elevation) < 1e-3) ? ((u_elevation >= 0.0) ? 1e-3 : -1e-3) : u_elevation;
    float se = sin(el), ce = cos(el);
    float3 fwd = float3(0.0, -se, ce), right = float3(1.0, 0.0, 0.0), up = float3(0.0, ce, se);

    const float D = 26.0;
    float3 pos = -fwd * D;
    float3 dir0 = normalize(fwd + (b.x / D) * right + (b.y / D) * up);
    float3 hv = cross(pos, dir0);
    float h2 = dot(hv, hv);
    float r0 = length(pos);
    float3 vel = dir0 * sqrt(1.0 + h2 / (r0 * r0 * r0));

    float T_ray = 1.0, T_bg = 1.0, holeShadow = 0.0;
    float3 E = float3(0.0, 0.0, 0.0);
    bool captured = false;
    int innerCrossings = 0;

    [loop]
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
            if (rh < u_diskIn + 0.42) {
                float sh = 1.0 - smoothstep(u_diskIn + 0.06, u_diskIn + 0.42, rh);
                holeShadow = max(holeShadow, sh * T_bg);
            }
            if (rh < u_diskIn * 0.98) innerCrossings += 1;

            float minR = (orderBefore > 0) ? (u_diskIn * 0.62) : u_diskIn;
            float maxR = (orderBefore > 0) ? (u_diskOut * 2.4) : (u_diskOut * 1.18);
            if (rh > minR && rh < maxR) {
                float rb = 0.0, bb = 0.0;
                float3 e = evalDisk(hit, hvel, orderBefore, rb, bb);
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

    float yAspect = lerp(1.0, 0.52, abs(ce));
    float2 bEll = float2(b.x * yAspect, b.y);
    float dGlow = max(length(bEll) - 2.8, 0.0);
    float dopSide = 0.72 - 0.28 * abs(ce) * clamp(b.x / 5.0, -1.0, 1.0);
    E += exp(-dGlow / 1.65) * 0.055 * dopSide * (1.0 - holeShadow) * float3(0.95, 0.16, 0.02);

    float edge = 1.0 - smoothstep(0.80, 0.97, max(abs(inp.p.x), abs(inp.p.y)) / u_extent);
    float3 xCol = E * u_exposure;
    float lum = max(max(xCol.r, xCol.g), xCol.b);
    float3 mapped = (lum > 1e-4) ? (xCol * ((1.0 - exp(-lum)) / lum)) : float3(0.0, 0.0, 0.0);
    mapped = lerp(mapped, 1.0 - exp(-xCol), smoothstep(1.1, 3.2, lum) * 0.42);
    float3 col = mapped * edge;

    float shadowCov = clamp(holeShadow, 0.0, 1.0) * edge;
    float visBg = T_bg * (1.0 - shadowCov);
    float4 back = float4(0.0, 0.0, 0.0, 0.0);
    if (visBg > 1e-3) {
        float2 bBg = float2(inp.p.x, -inp.p.y) * u_rsPerPoint;
        float blBg = length(bBg);
        float taper = 1.0 - smoothstep(0.55, 0.8, blBg / (u_extent * u_rsPerPoint));
        float2 src = bBg * (1.0 - u_lensE * u_lensE / max(blBg * blBg, 1e-3) * taper);
        float2 uv = (float2(src.x, -src.y) / u_rsPerPoint + u_extent) / (2.0 * u_extent);
        back = tex0.SampleLevel(smp0, uv, 0.0);
    }
    float occl = clamp(1.0 - T_bg, 0.0, 1.0) * edge;
    float alpha = clamp(occl + T_bg * shadowCov + visBg * back.a, 0.0, 1.0);
    return float4(col + visBg * back.rgb, alpha);
}

struct IconOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
IconOut icon_vs(uint vid : SV_VertexID) {
    float2 corner = float2(float(vid & 1u), float(vid >> 1u));
    float2 scene = it_center + rot2((corner - 0.5) * it_size, it_rotation);
    IconOut o;
    o.pos = float4(toNdc(scene), 0.0, 1.0);
    o.uv = corner;
    return o;
}
float cellThreshold(float2 cu) {
    float2 scene = it_center + rot2((cu - 0.5) * it_fit * it_size, it_rotation);
    float2 toHole = u_center - it_center;
    float l = length(toHole);
    float facing = (l > 1e-3) ? dot(normalize(scene - it_center + 1e-4), toHole / l) : 0.0;
    return 0.6 * (0.5 - 0.5 * facing) + 0.4 * hash12(floor(cu * 24.0) + it_seed * 17.0);
}
float erodeLevel() { return max(it_tidal * 0.35, it_consume / 0.55); }
float4 icon_ps(IconOut inp) : SV_Target {
    float2 iu = (inp.uv - 0.5) / it_fit + 0.5;
    float4 c = float4(0.0, 0.0, 0.0, 0.0);
    if (all(iu >= 0.0) && all(iu <= 1.0)) c = tex0.Sample(smp0, iu);
    float level = erodeLevel();
    if (level > 0.0) {
        float thr = cellThreshold((floor(clamp(iu, 0.0, 0.9999) * 24.0) + 0.5) / 24.0);
        thr += (hash12(floor(iu * 48.0) + it_seed) - 0.5) * 0.06;
        if (thr < level) c = float4(0.0, 0.0, 0.0, 0.0);
        else if (thr < level + 0.08)
            c.rgb = lerp(c.rgb, float3(1.0, 0.5, 0.15) * c.a, 0.8 * (1.0 - (thr - level) / 0.08));
    }
    if (it_outlined > 0.5) {
        float2 px = inp.uv * it_size, d2 = min(px, it_size - px);
        float a = 1.0 - clamp(level * 2.0, 0.0, 1.0);
        if (min(d2.x, d2.y) < 1.5) c = lerp(c, float4(1.0, 0.58, 0.0, 1.0) * a, a);
    }
    return c * it_opacity;
}

struct PartOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
PartOut particle_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    PartOut o;
    o.pos = float4(2.0, 2.0, 0.0, 1.0);
    o.uv = float2(0.0, 0.0);
    o.color = float4(0.0, 0.0, 0.0, 0.0);
    float2 cell = float2(float(iid % 24u), float(iid / 24u));
    float2 cu = (cell + 0.5) / 24.0;
    float4 ic = tex0.SampleLevel(smp0, cu, 0.0);
    if (ic.a < 0.15) return o;
    float thr = cellThreshold(cu), e = 0.0;
    if (it_consume > 0.0) {
        float tr = thr * 0.55;
        if (it_consume <= tr) return o;
        e = clamp((it_consume - tr) / 0.45, 0.0, 1.0);
    } else {
        if (thr >= it_tidal * 0.35) return o;
        e = fract1(u_time / 1.4 + hash12(cell * 1.7 + it_seed));
    }
    if (e >= 1.0) return o;
    float2 d0 = it_center + rot2((cu - 0.5) * it_fit * it_size, it_rotation) - u_center;
    float2 ld0 = rot2(d0, -u_tilt);
    float jitter = hash12(cell + 3.1) - 0.5;
    float spinSign = (u_elevation >= 0.0) ? -1.0 : 1.0;
    float2 lv = rot2(ld0, spinSign * (e * e * 2.6 + e * 0.7) * (1.0 + 0.3 * jitter)) * pows(1.0 - e, 1.35);
    float squash = max(abs(sin(u_elevation)), 0.16);
    lv.y *= lerp(1.0, squash, smoothstep(0.0, 0.85, e));
    float a = ic.a * smoothstep(0.0, 0.06, e) * (1.0 - smoothstep(0.7, 1.0, e));
    float farSide = (u_elevation >= 0.0) ? -lv.y : lv.y;
    if (farSide > 0.0)
        a *= lerp(smoothstep(u_radius * 1.05, u_radius * 1.32, length(lv)), 1.0,
                  smoothstep(0.55, 0.9, abs(sin(u_elevation))));
    float2 v = rot2(lv, u_tilt);
    float3 col = lerp(ic.rgb / max(ic.a, 1e-3), float3(1.0, 0.5, 0.18), smoothstep(0.1, 0.6, e)) * (1.0 + 1.2 * e);
    float size = lerp(1.5, 0.55, e) * (0.7 + 0.6 * hash12(cell + 9.2)) * clamp(it_size.x / 48.0, 0.6, 2.0);
    float2 corner = float2(float(vid & 1u), float(vid >> 1u));
    o.pos = float4(toNdc(u_center + v + (corner - 0.5) * 2.0 * size), 0.0, 1.0);
    o.uv = corner;
    o.color = float4(col * a, 0.0);
    return o;
}
float4 particle_ps(PartOut inp) : SV_Target {
    float2 q = inp.uv * 2.0 - 1.0;
    return inp.color * exp(-dot(q, q) * 2.5);
}
)HLSL";

// Restores immediate-context bindings so ID2D1DeviceContext state stays untouched.
struct ContextGuard {
    ID3D11DeviceContext* ctx;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0xffffffffu;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;
    UINT numViewports = 1;
    D3D11_VIEWPORT viewport{};
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vsCb[2], psCb[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> vsSrv, psSrv;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> vsSmp, psSmp;

    explicit ContextGuard(ID3D11DeviceContext* c) : ctx(c) {
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->RSGetState(&raster);
        ctx->RSGetViewports(&numViewports, &viewport);
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->IAGetInputLayout(&layout);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ID3D11Buffer* vcb[2]{};
        ctx->VSGetConstantBuffers(0, 2, vcb);
        vsCb[0].Attach(vcb[0]);
        vsCb[1].Attach(vcb[1]);
        ID3D11Buffer* pcb[2]{};
        ctx->PSGetConstantBuffers(0, 2, pcb);
        psCb[0].Attach(pcb[0]);
        psCb[1].Attach(pcb[1]);
        ctx->VSGetShaderResources(0, 1, &vsSrv);
        ctx->PSGetShaderResources(0, 1, &psSrv);
        ctx->VSGetSamplers(0, 1, &vsSmp);
        ctx->PSGetSamplers(0, 1, &psSmp);
    }
    ~ContextGuard() {
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        ctx->VSSetShaderResources(0, 1, &nullSrv);
        ID3D11RenderTargetView* r = rtv.Get();
        ctx->OMSetRenderTargets(1, &r, dsv.Get());
        ctx->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        ctx->RSSetState(raster.Get());
        if (numViewports > 0) ctx->RSSetViewports(1, &viewport);
        ctx->IASetPrimitiveTopology(topology);
        ctx->IASetInputLayout(layout.Get());
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* vcb[2] = {vsCb[0].Get(), vsCb[1].Get()};
        ctx->VSSetConstantBuffers(0, 2, vcb);
        ID3D11Buffer* pcb[2] = {psCb[0].Get(), psCb[1].Get()};
        ctx->PSSetConstantBuffers(0, 2, pcb);
        ID3D11ShaderResourceView* vsrv = vsSrv.Get();
        ctx->VSSetShaderResources(0, 1, &vsrv);
        ID3D11ShaderResourceView* psrv = psSrv.Get();
        ctx->PSSetShaderResources(0, 1, &psrv);
        ID3D11SamplerState* vsmp = vsSmp.Get();
        ctx->VSSetSamplers(0, 1, &vsmp);
        ID3D11SamplerState* psmp = psSmp.Get();
        ctx->PSSetSamplers(0, 1, &psmp);
    }
    ContextGuard(const ContextGuard&) = delete;
    ContextGuard& operator=(const ContextGuard&) = delete;
};

}  // namespace bhd3d

class BlackHoleD3D11 {
   public:
    enum class Claim { None, Background, Foreground };

    void clearIcons() { icons_.clear(); }
    void cacheIcon(ID3D11Device* device, IWICFormatConverter* converter) {
        if (!device || !converter) {
            icons_.push_back({});
            return;
        }
        UINT w = 0, h = 0;
        if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || w > 1024 || h > 1024) {
            icons_.push_back({});
            return;
        }
        std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) {
            icons_.push_back({});
            return;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateTexture2D(&td, &init, &tex)) ||
            FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
            icons_.push_back({});
            return;
        }
        icons_.push_back({std::move(srv), w, h});
    }

    bool begin(ID3D11Device* device, ID2D1DeviceContext* d2d, const BlackHoleItem& hole, float dpiScale) {
        bg_.clear();
        fg_.clear();
        active_ = false;
        if (!device || !d2d || !std::isfinite(hole.radius) || hole.radius <= 0 || !hole.center.isFinite())
            return false;
        if (!ensurePipeline(device)) return false;

        const float extent = hole.radius * bhd3d::kRegionRadii;
        const float px = std::min(std::min(std::max(dpiScale, 1.0f), bhd3d::kMaxPixelScale),
                                  bhd3d::kMaxDrawable / (2.0f * extent));
        const UINT side = std::max<UINT>(64u, static_cast<UINT>(std::lround(2.0f * extent * px)));
        if (!ensureTargets(device, d2d, side)) return false;

        uniforms_ = {};
        uniforms_.centerX = hole.center.x;
        uniforms_.centerY = hole.center.y;
        uniforms_.originX = hole.center.x - extent;
        uniforms_.originY = hole.center.y - extent;
        uniforms_.extent = extent;
        uniforms_.radius = hole.radius;
        uniforms_.time = std::isfinite(hole.time) ? hole.time : 0.0f;
        uniforms_.rsPerPoint = 2.598f / hole.radius;
        uniforms_.elevation =
            std::isfinite(hole.elevation) ? std::clamp(hole.elevation, -1.4835298f, 1.4835298f) : .056f;
        uniforms_.exposure = 1.25f;
        uniforms_.diskIn = 3.0f;
        uniforms_.diskOut = 13.2f;
        uniforms_.lensE = 3.0f;
        uniforms_.tilt = std::isfinite(hole.tilt) ? hole.tilt : 0.0f;
        active_ = true;
        return true;
    }

    Claim claim(const SpriteItem& s) {
        if (!active_ || !s.center.isFinite() || !s.size.isFinite()) return Claim::None;
        const auto index = static_cast<std::size_t>(s.texture);
        if (index == 0 || index > icons_.size() || !icons_[index - 1].srv) return Claim::None;

        const Vec2 c{uniforms_.centerX, uniforms_.centerY};
        Claim kind = Claim::None;
        if (s.consume > 0)
            kind = Claim::Foreground;
        else if (s.grabbed)
            kind = s.tidal > 0 ? Claim::Foreground : Claim::None;
        else if ((s.center - c).length() + .5f * s.size.length() < uniforms_.extent * .97f)
            kind = Claim::Background;
        if (kind == Claim::None || bg_.size() + fg_.size() >= bhd3d::kMaxCopies) return Claim::None;
        if (s.consume >= 1.0f || s.opacity <= 0.0f) return kind;

        const auto& icon = icons_[index - 1];
        const float imageAspect =
            static_cast<float>(icon.w) / std::max<float>(1.0f, static_cast<float>(icon.h));
        const float sizeAspect = s.size.x / std::max(1e-3f, s.size.y);
        bhd3d::Instance it{};
        it.centerX = s.center.x;
        it.centerY = s.center.y;
        it.sizeX = s.size.x;
        it.sizeY = s.size.y;
        if (imageAspect > sizeAspect) {
            it.fitX = 1.0f;
            it.fitY = sizeAspect / imageAspect;
        } else {
            it.fitX = imageAspect / sizeAspect;
            it.fitY = 1.0f;
        }
        it.rotation = std::isfinite(s.rotation) ? s.rotation : 0.0f;
        it.tidal = std::clamp(s.tidal, 0.0f, 1.0f);
        it.consume = std::clamp(s.consume, 0.0f, 1.0f);
        it.outlined = 0.0f;
        it.seed = static_cast<float>(index % 977u);
        it.opacity = std::clamp(s.opacity, 0.0f, 1.0f);
        (kind == Claim::Background ? bg_ : fg_).push_back({it, icon.srv.Get()});
        return kind;
    }

    void renderToTarget(ID3D11Device* device) {
        if (!active_ || !device) return;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        if (!ctx) return;

        bhd3d::ContextGuard guard(ctx.Get());
        D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(side_), static_cast<float>(side_), 0.0f, 1.0f};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster_.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->UpdateSubresource(uniformBuf_.Get(), 0, nullptr, &uniforms_, 0, 0);
        ID3D11Buffer* ubuf = uniformBuf_.Get();
        ctx->VSSetConstantBuffers(0, 1, &ubuf);
        ctx->PSSetConstantBuffers(0, 1, &ubuf);
        ID3D11SamplerState* smp = sampler_.Get();
        ctx->VSSetSamplers(0, 1, &smp);
        ctx->PSSetSamplers(0, 1, &smp);

        const FLOAT clearZero[4] = {0, 0, 0, 0};
        const FLOAT blendFactors[4] = {0, 0, 0, 0};

        // 1. Offscreen lensable icon copies.
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        ID3D11RenderTargetView* bgRtv = bgRtv_.Get();
        ctx->OMSetRenderTargets(1, &bgRtv, nullptr);
        ctx->ClearRenderTargetView(bgRtv, clearZero);
        ctx->OMSetBlendState(blendPremul_.Get(), blendFactors, 0xffffffffu);
        drawCopies(ctx.Get(), bg_);

        // 2. Schwarzschild ray-traced hole + lensed background copies.
        ID3D11RenderTargetView* outRtv = outRtv_.Get();
        ctx->OMSetRenderTargets(1, &outRtv, nullptr);
        ctx->ClearRenderTargetView(outRtv, clearZero);
        ctx->OMSetBlendState(nullptr, blendFactors, 0xffffffffu);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(holeVs_.Get(), nullptr, 0);
        ctx->PSSetShader(holePs_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* bgSrv = bgSrv_.Get();
        ctx->PSSetShaderResources(0, 1, &bgSrv);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nullSrv);

        // 3. Foreground dissolving/grabbed copies + 576 spiralling particles per copy.
        ctx->OMSetBlendState(blendPremul_.Get(), blendFactors, 0xffffffffu);
        drawCopies(ctx.Get(), fg_);

        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(partVs_.Get(), nullptr, 0);
        ctx->PSSetShader(partPs_.Get(), nullptr, 0);
        ID3D11Buffer* ibuf = instanceBuf_.Get();
        ctx->VSSetConstantBuffers(1, 1, &ibuf);
        for (const auto* list : {&bg_, &fg_}) {
            for (const auto& copy : *list) {
                if (copy.instance.tidal <= 0.0f && copy.instance.consume <= 0.0f) continue;
                ctx->UpdateSubresource(instanceBuf_.Get(), 0, nullptr, &copy.instance, 0, 0);
                ctx->VSSetShaderResources(0, 1, &copy.srv);
                ctx->DrawInstanced(4, bhd3d::kParticlesPerCopy, 0, 0);
            }
        }
        ctx->VSSetShaderResources(0, 1, &nullSrv);
    }

    [[nodiscard]] bool active() const { return active_ && outBitmap_ != nullptr; }
    [[nodiscard]] ID2D1Bitmap1* bitmap() const { return outBitmap_.Get(); }
    [[nodiscard]] D2D1_RECT_F destRect() const {
        return D2D1::RectF(uniforms_.originX, uniforms_.originY, uniforms_.originX + 2.0f * uniforms_.extent,
                           uniforms_.originY + 2.0f * uniforms_.extent);
    }

   private:
    struct IconTex {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        UINT w = 0, h = 0;
    };
    struct Copy {
        bhd3d::Instance instance;
        ID3D11ShaderResourceView* srv;
    };

    bool compiled_ = false, failed_ = false, active_ = false;
    UINT side_ = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> holeVs_, iconVs_, partVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> holePs_, iconPs_, partPs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> uniformBuf_, instanceBuf_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendPremul_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bgTex_, outTex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bgRtv_, outRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> bgSrv_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> outBitmap_;
    std::vector<IconTex> icons_;
    std::vector<Copy> bg_, fg_;
    bhd3d::Uniforms uniforms_{};

    static Microsoft::WRL::ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
        Microsoft::WRL::ComPtr<ID3DBlob> code, err;
        const HRESULT hr = D3DCompile(bhd3d::kHlsl, std::strlen(bhd3d::kHlsl), "black_hole.hlsl", nullptr,
                                      nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (FAILED(hr)) return nullptr;
        return code;
    }

    bool ensurePipeline(ID3D11Device* device) {
        if (compiled_) return !failed_;
        compiled_ = true;
        if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            failed_ = true;
            return false;
        }
        auto hvs = compile("hole_vs", "vs_5_0"), hps = compile("hole_ps", "ps_5_0");
        auto ivs = compile("icon_vs", "vs_5_0"), ips = compile("icon_ps", "ps_5_0");
        auto pvs = compile("particle_vs", "vs_5_0"), pps = compile("particle_ps", "ps_5_0");
        if (!hvs || !hps || !ivs || !ips || !pvs || !pps ||
            FAILED(device->CreateVertexShader(hvs->GetBufferPointer(), hvs->GetBufferSize(), nullptr,
                                              &holeVs_)) ||
            FAILED(device->CreatePixelShader(hps->GetBufferPointer(), hps->GetBufferSize(), nullptr,
                                             &holePs_)) ||
            FAILED(device->CreateVertexShader(ivs->GetBufferPointer(), ivs->GetBufferSize(), nullptr,
                                              &iconVs_)) ||
            FAILED(device->CreatePixelShader(ips->GetBufferPointer(), ips->GetBufferSize(), nullptr,
                                             &iconPs_)) ||
            FAILED(device->CreateVertexShader(pvs->GetBufferPointer(), pvs->GetBufferSize(), nullptr,
                                              &partVs_)) ||
            FAILED(device->CreatePixelShader(pps->GetBufferPointer(), pps->GetBufferSize(), nullptr,
                                             &partPs_))) {
            failed_ = true;
            return false;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(bhd3d::Uniforms);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &uniformBuf_))) {
            failed_ = true;
            return false;
        }
        bd.ByteWidth = sizeof(bhd3d::Instance);
        if (FAILED(device->CreateBuffer(&bd, nullptr, &instanceBuf_))) {
            failed_ = true;
            return false;
        }
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &sampler_))) {
            failed_ = true;
            return false;
        }
        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&bld, &blendPremul_))) {
            failed_ = true;
            return false;
        }
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        if (FAILED(device->CreateRasterizerState(&rd, &raster_))) {
            failed_ = true;
            return false;
        }
        return true;
    }

    bool ensureTargets(ID3D11Device* device, ID2D1DeviceContext* d2d, UINT side) {
        if (side_ == side && outBitmap_ && bgRtv_ && outRtv_ && bgSrv_) return true;
        outBitmap_.Reset();
        bgSrv_.Reset();
        bgRtv_.Reset();
        outRtv_.Reset();
        bgTex_.Reset();
        outTex_.Reset();
        side_ = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = side;
        td.Height = side;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &bgTex_)) ||
            FAILED(device->CreateRenderTargetView(bgTex_.Get(), nullptr, &bgRtv_)) ||
            FAILED(device->CreateShaderResourceView(bgTex_.Get(), nullptr, &bgSrv_)) ||
            FAILED(device->CreateTexture2D(&td, nullptr, &outTex_)) ||
            FAILED(device->CreateRenderTargetView(outTex_.Get(), nullptr, &outRtv_)))
            return false;

        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (FAILED(outTex_.As(&surface))) return false;
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(d2d->CreateBitmapFromDxgiSurface(surface.Get(), &props, &outBitmap_))) return false;
        side_ = side;
        return true;
    }

    void drawCopies(ID3D11DeviceContext* ctx, const std::vector<Copy>& copies) {
        if (copies.empty()) return;
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(iconVs_.Get(), nullptr, 0);
        ctx->PSSetShader(iconPs_.Get(), nullptr, 0);
        ID3D11Buffer* ibuf = instanceBuf_.Get();
        ctx->VSSetConstantBuffers(1, 1, &ibuf);
        ctx->PSSetConstantBuffers(1, 1, &ibuf);
        for (const auto& copy : copies) {
            ctx->UpdateSubresource(instanceBuf_.Get(), 0, nullptr, &copy.instance, 0, 0);
            ctx->PSSetShaderResources(0, 1, &copy.srv);
            ctx->Draw(4, 0);
        }
    }
};

}  // namespace dpp::winmirror
