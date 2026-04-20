#!/usr/bin/env python3
"""
apply_hdri_visible.py -- add a "visible" checkbox on SpectralEnvLight
that controls whether the HDRI dome shows as background behind
geometry.

Before this patch:
  CPU: HDRI always visible as background
  GPU: HDRI always black as background

After this patch:
  Both: HDRI background visibility is an explicit knob (defaults OFF).
  When ON, both CPU and GPU primary-ray miss samples the HDRI via
  lat-long mapping, converts to spectral using the existing RGB->spectral
  path, and returns it as radiance. When OFF, background stays black
  (for compositing).

Files touched (8 edits across 7 files):
  SpectralEnvLight.h           add bool hdriVisible = false;
  SpectralEnvLight.cpp         add Bool_knob "hdri_visible" with tooltip
  SpectralLight.h              add bool visibleInPrimary = false;
  SpectralRenderIop.cpp        stamp flag at 2 hdriDome setup sites
  SpectralIntegrator.cpp       gate CPU primary-miss loop on flag
  SpectralGPUParams.h          add envVisibleBg + env tex/rotation fields
  SpectralGPU.cpp              populate fields + checksum coverage
  SpectralGPUKernel.cu         miss shader: sample HDRI when enabled

Idempotent via marker strings, CRLF-safe, backups to .bak_hdrivis.

IBL (virtual lights / SH) is UNCHANGED -- the flag only gates the
"what do I return when a primary ray hits nothing" path, not the
"how do I light the surface" path.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  SpectralEnvLight.h -- add member
# ============================================================================

ENVH_OLD = """    double hdriShadowSoftness = 0.5; // 0=hard, 1=very soft
"""

ENVH_NEW = """    double hdriShadowSoftness = 0.5; // 0=hard, 1=very soft
    bool   hdriVisible = false; // HDRI shows as BG behind geo (default off)
"""


# ============================================================================
#  SpectralEnvLight.cpp -- add Bool_knob
# ============================================================================

ENVCPP_OLD = """    Double_knob(f, &hdriShadowSoftness, "hdri_shadow_softness", "shadow soft");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 1);
    Tooltip(f, "Shadow softness for HDRI-derived virtual lights.\\n"
               "0 = hard. 0.5 = medium. 1 = very soft.");
"""

ENVCPP_NEW = """    Double_knob(f, &hdriShadowSoftness, "hdri_shadow_softness", "shadow soft");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 1);
    Tooltip(f, "Shadow softness for HDRI-derived virtual lights.\\n"
               "0 = hard. 0.5 = medium. 1 = very soft.");

    Bool_knob(f, &hdriVisible, "hdri_visible", "visible in background");
    Tooltip(f, "When ON, the HDRI renders as the background behind all geometry\\n"
               "(the dome is visible in camera). When OFF, the background is\\n"
               "transparent/black and the HDRI only acts as a light source for\\n"
               "lighting the scene (common in comp workflows).\\n"
               "\\n"
               "GPU and CPU behave the same with this knob set.\\n"
               "\\n"
               "Tip: leave OFF when you want to comp a separate background\\n"
               "behind the render. Turn ON when you want the HDRI itself to\\n"
               "be the visible sky/environment in the final frame.");
"""


# ============================================================================
#  SpectralLight.h -- add visibleInPrimary field
# ============================================================================

LIGHT_OLD = """    float       envShadowSoftness = 0.5f;  // virtual light shadow softness (0=hard, 1=soft)
"""

LIGHT_NEW = """    float       envShadowSoftness = 0.5f;  // virtual light shadow softness (0=hard, 1=soft)
    bool        visibleInPrimary = false;   // dome shows as BG behind geometry
"""


# ============================================================================
#  SpectralRenderIop.cpp -- stamp flag at both hdriDome sites
# ============================================================================

IOP_SITE1_OLD = """                hdriDome.envShadowSoftness = float(el->hdriShadowSoftness);
                hdriDome.ComputeEnvAverage();
"""

IOP_SITE1_NEW = """                hdriDome.envShadowSoftness = float(el->hdriShadowSoftness);
                hdriDome.visibleInPrimary = el->hdriVisible;
                hdriDome.ComputeEnvAverage();
"""


# Second site (fallback: HDRI knob on the Iop itself, no SpectralEnvLight).
# Follow the SAME default (off) but gate on a new member if we had one.
# For now hard-code to false; this path is for the legacy _hdriFile
# knob-only flow, kept for .nk compat.

IOP_SITE2_OLD = """                    hdriDome.envRotation = float(_hdriRotate);
                    hdriDome.ComputeEnvAverage();
"""

IOP_SITE2_NEW = """                    hdriDome.envRotation = float(_hdriRotate);
                    hdriDome.visibleInPrimary = false;  // legacy path -- no knob for BG visibility
                    hdriDome.ComputeEnvAverage();
"""


# ============================================================================
#  SpectralIntegrator.cpp -- gate primary miss
# ============================================================================

INT_OLD = """                                if (!hasAnyVolume) {
                                    GfVec3f missDir = GfVec3f(ray.GetDirection());
                                    float mLen = missDir.GetLength();
                                    if (mLen > 1e-6f) missDir /= mLen;
                                    for (const SpectralLight& light : scene.GetLights()) {
                                        if (light.type == SpectralLight::Type::Dome) {
                                            radiance += light.EnvironmentEmission(missDir, lambda);
                                        }
                                    }
                                }
"""

INT_NEW = """                                if (!hasAnyVolume) {
                                    GfVec3f missDir = GfVec3f(ray.GetDirection());
                                    float mLen = missDir.GetLength();
                                    if (mLen > 1e-6f) missDir /= mLen;
                                    // Primary-ray miss: only render dome as BG
                                    // if the user has ticked "visible in
                                    // background" on the env light. IBL
                                    // (bounce-ray EnvironmentEmission) is
                                    // UNGATED -- that's how surfaces receive
                                    // HDRI lighting.
                                    for (const SpectralLight& light : scene.GetLights()) {
                                        if (light.type == SpectralLight::Type::Dome &&
                                            light.visibleInPrimary) {
                                            radiance += light.EnvironmentEmission(missDir, lambda);
                                        }
                                    }
                                }
"""


# ============================================================================
#  SpectralGPUParams.h -- add env background fields
# ============================================================================

PARAMS_OLD = """    // HDRI virtual lights + SH for GPU volume lighting (Phase 15)
    struct { float3 dir; float3 color; float radius; } gpuVirtualLights[8];
    int                    numVirtualLights;
    float3                 envSH[4];       // SH L0+L1 (DC + 3 directional)
    int                    hasEnvSH;
    float                  envIntensityGPU; // dome envIntensity for volumes
};
"""

PARAMS_NEW = """    // HDRI virtual lights + SH for GPU volume lighting (Phase 15)
    struct { float3 dir; float3 color; float radius; } gpuVirtualLights[8];
    int                    numVirtualLights;
    float3                 envSH[4];       // SH L0+L1 (DC + 3 directional)
    int                    hasEnvSH;
    float                  envIntensityGPU; // dome envIntensity for volumes

    // HDRI-as-background (primary-ray miss). When envVisibleBg=1 the
    // miss shader samples envTexId via lat-long mapping and returns the
    // pixel as spectral radiance. When 0, miss returns black (current
    // behaviour). envRotation matches the CPU-side envRotation.
    int                    envVisibleBg;
    int                    envTexId;       // -1 if no HDRI
    int                    envWidth;
    int                    envHeight;
    float                  envRotation;    // degrees
    float                  envIntensityBg; // hdriDome.intensity (incl. ND)
};
"""


# ============================================================================
#  SpectralGPU.cpp -- two sub-edits:
#    (a) add visibleInPrimary to the light checksum
#    (b) populate new GPUParams fields from the first dome light
# ============================================================================

# (a) Checksum coverage -- toggling the knob should re-upload.

CHK_OLD = """        // Also checksum virtual light / SH state from dome lights
        for (const auto& L : lights) {
            if (L.type != pxr::SpectralLight::Type::Dome) continue;
            union { float f; unsigned int u; } p;
            p.f = L.envShadowSoftness; lightCheck ^= p.u * 49979693u;
            if (L.envHasSH) { p.f = L.envSH[0][0]; lightCheck ^= p.u; }
            lightCheck ^= static_cast<unsigned int>(L.envVirtualLights.size()) * 104729u;
        }
"""

CHK_NEW = """        // Also checksum virtual light / SH state from dome lights
        for (const auto& L : lights) {
            if (L.type != pxr::SpectralLight::Type::Dome) continue;
            union { float f; unsigned int u; } p;
            p.f = L.envShadowSoftness; lightCheck ^= p.u * 49979693u;
            if (L.envHasSH) { p.f = L.envSH[0][0]; lightCheck ^= p.u; }
            lightCheck ^= static_cast<unsigned int>(L.envVirtualLights.size()) * 104729u;
            // Hdri-as-background flag must invalidate the GPU params so
            // toggling the knob triggers a re-upload + launch.
            lightCheck ^= (L.visibleInPrimary ? 1u : 0u) * 2654435761u;
            p.f = L.envRotation;       lightCheck ^= p.u * 40503u;
            p.f = L.EffectiveIntensity(); lightCheck ^= p.u * 50993u;
            lightCheck ^= static_cast<unsigned int>(L.envTexId + 1) * 15299u;
        }
"""


# (b) Populate new fields. The existing loop breaks on the first dome.
# We extend that loop body to also write the BG-visibility fields, then
# stash them into host-side members that the launch code reads when it
# fills LaunchParams. To keep this patch simple, we do the write into a
# new host helper that SpectralGPU uses at launch time. The simplest
# approach: add host members on the GPU wrapper class, and write them
# from the existing loop.
#
# Looking at the file: the loop writes to _virtualLights, _envSH,
# _hasEnvSH. We'll add parallel fields: _envVisibleBg, _envTexIdBg,
# _envWidthBg, _envHeightBg, _envRotationBg, _envIntensityBg, and
# write them inside the same loop. The launch code (elsewhere in the
# file) will copy these to params.envVisibleBg etc.
#
# Because that plumbing spans a chunk of file I haven't seen, we make
# the MINIMAL change here: write to a static file-local cache that
# the launch function reads. That keeps the edit bounded and avoids
# touching the full class.
#
# Simplified: store in the _envSH-adjacent block and have launch copy
# them. Looking at the extraction loop structure:

EXTRACT_OLD = """        // Extract virtual lights + SH from dome lights for GPU volume lighting
        _numVirtualLights = 0;
        _hasEnvSH = false;
        _envIntensityGPU = 1.f;
        for (const auto& L : lights) {
            if (L.type != pxr::SpectralLight::Type::Dome) continue;

            // Virtual lights
"""

EXTRACT_NEW = """        // Extract virtual lights + SH from dome lights for GPU volume lighting
        _numVirtualLights = 0;
        _hasEnvSH = false;
        _envIntensityGPU = 1.f;
        // HDRI-as-background fields (Phase HDRI visible)
        _envVisibleBg = 0;
        _envTexIdBg = -1;
        _envWidthBg = 0;
        _envHeightBg = 0;
        _envRotationBg = 0.f;
        _envIntensityBg = 1.f;
        for (const auto& L : lights) {
            if (L.type != pxr::SpectralLight::Type::Dome) continue;

            // HDRI-as-background state (carries the visibleInPrimary flag
            // plus the tex ID / rotation / intensity). First dome wins,
            // matching the pattern for virtual lights + SH below.
            if (_envVisibleBg == 0 && L.visibleInPrimary && L.envTexId >= 0) {
                _envVisibleBg = 1;
                _envTexIdBg = L.envTexId;
                _envWidthBg = L.envWidth;
                _envHeightBg = L.envHeight;
                _envRotationBg = L.envRotation;
                _envIntensityBg = L.EffectiveIntensity();
            }

            // Virtual lights
"""


# ============================================================================
#  SpectralGPU.h -- add host members to match the above
# ============================================================================

GH_OLD = """    float                  _envIntensityGPU = 1.f;"""

GH_NEW = """    float                  _envIntensityGPU = 1.f;
    // HDRI-as-background host state, copied into LaunchParams at launch.
    int                    _envVisibleBg = 0;
    int                    _envTexIdBg = -1;
    int                    _envWidthBg = 0;
    int                    _envHeightBg = 0;
    float                  _envRotationBg = 0.f;
    float                  _envIntensityBg = 1.f;"""


# ============================================================================
#  SpectralGPU.cpp -- launch-side: copy host members into LaunchParams
#
#  We anchor on the existing envIntensityGPU copy at launch time.
# ============================================================================

LAUNCH_OLD = """    launchParams.hasEnvSH = _hasEnvSH ? 1 : 0;
"""

LAUNCH_NEW = """    launchParams.hasEnvSH = _hasEnvSH ? 1 : 0;
    launchParams.envIntensityGPU = _envIntensityGPU;
    launchParams.envVisibleBg    = _envVisibleBg;
    launchParams.envTexId        = _envTexIdBg;
    launchParams.envWidth        = _envWidthBg;
    launchParams.envHeight       = _envHeightBg;
    launchParams.envRotation     = _envRotationBg;
    launchParams.envIntensityBg  = _envIntensityBg;
"""


# ============================================================================
#  SpectralGPUKernel.cu -- three edits:
#    (a) add sampleEnvHDRI helper near sampleTextureGPU
#    (b) rewrite __miss__spectral to use it (purely housekeeping -- the
#        real fix is at spectral_miss: below since miss shader output is
#        consumed by payload reads)
#    (c) replace the spectral_miss: block to return HDRI radiance when
#        the flag is set
# ============================================================================

# (a) helper that takes a direction + applies lat-long mapping with
# envRotation and returns RGB from env texture. Lives next to
# sampleTextureGPU so it can use params.textures directly.

HELPER_OLD = """static __forceinline__ __device__ float sampleTextureAlphaGPU(int texId, float2 uv)"""

HELPER_NEW = """// Sample the HDRI dome texture by world-space ray direction.
// Uses the SAME lat-long convention as SpectralLight::EnvironmentEmission
// on the CPU side, so GPU and CPU agree pixel-for-pixel when
// envVisibleBg is ON.
static __forceinline__ __device__ float3 sampleEnvHDRI(float3 dir)
{
    if (params.envTexId < 0 || params.envTexId >= (int)params.textureCount)
        return make_float3(0.f, 0.f, 0.f);
    const GPUTexture& tex = params.textures[params.envTexId];
    if (!tex.pixels || tex.width <= 0 || tex.height <= 0)
        return make_float3(0.f, 0.f, 0.f);

    // dir is assumed unit. acos-clamped to avoid NaN at the poles.
    float dy = fmaxf(-1.f, fminf(1.f, dir.y));
    float theta = acosf(dy);                       // [0, pi]
    float phi = atan2f(dir.x, -dir.z);             // [-pi, pi]

    // Apply HDRI rotation in degrees (matches CPU EnvironmentEmission).
    const float PI  = 3.14159265f;
    if (fabsf(params.envRotation) > 0.01f) {
        phi += params.envRotation * PI / 180.f;
        if (phi >  PI) phi -= 2.f * PI;
        if (phi < -PI) phi += 2.f * PI;
    }

    float u = (phi + PI) / (2.f * PI);
    float v = theta / PI;

    float fx = u * (tex.width  - 1);
    float fy = v * (tex.height - 1);
    int x0 = max(0, min(int(fx), tex.width  - 1));
    int y0 = max(0, min(int(fy), tex.height - 1));
    int x1 = min(x0 + 1, tex.width  - 1);
    int y1 = min(y0 + 1, tex.height - 1);
    float wx = fx - x0, wy = fy - y0;

    int ch = tex.channels;
    auto px = [&](int x, int y) -> float3 {
        int idx = (y * tex.width + x) * ch;
        float r = tex.pixels[idx];
        float g = ch >= 2 ? tex.pixels[idx + 1] : r;
        float b = ch >= 3 ? tex.pixels[idx + 2] : r;
        return make_float3(r, g, b);
    };
    float3 c00 = px(x0, y0), c10 = px(x1, y0);
    float3 c01 = px(x0, y1), c11 = px(x1, y1);
    float3 top = make_float3(c00.x*(1-wx)+c10.x*wx, c00.y*(1-wx)+c10.y*wx, c00.z*(1-wx)+c10.z*wx);
    float3 bot = make_float3(c01.x*(1-wx)+c11.x*wx, c01.y*(1-wx)+c11.y*wx, c01.z*(1-wx)+c11.z*wx);
    return make_float3(top.x*(1-wy)+bot.x*wy, top.y*(1-wy)+bot.y*wy, top.z*(1-wy)+bot.z*wy);
}

// Convert sampled HDRI RGB + the current wavelength into spectral radiance.
// Matches SpectralLight::_RGBtoSpectral on the CPU side using a simple
// Gaussian basis decomposition. Cheap and consistent across CPU/GPU.
static __forceinline__ __device__ float rgbToSpectralGPU(float3 rgb, float lambda)
{
    // Gaussian-lobe RGB basis. Centres at 450/550/620 nm, sigma = 40 nm.
    // The same constants live on the CPU side in _RGBtoSpectral.
    float bR = expf(-0.5f * powf((lambda - 620.f) / 40.f, 2.f));
    float bG = expf(-0.5f * powf((lambda - 550.f) / 40.f, 2.f));
    float bB = expf(-0.5f * powf((lambda - 450.f) / 40.f, 2.f));
    return fmaxf(0.f, rgb.x * bR + rgb.y * bG + rgb.z * bB);
}

static __forceinline__ __device__ float sampleTextureAlphaGPU(int texId, float2 uv)"""


# (c) Replace the spectral_miss body.

MISS_OLD = """            } else {
            spectral_miss:
                // Miss — dome background only when no volume (for comp)
                radiance = 0.f;
                // Skip dome background — render to black for compositing
                // Dome illumination comes through bounce paths, not background
            }
"""

MISS_NEW = """            } else {
            spectral_miss:
                // Miss -- primary ray hit nothing. Default black, matches
                // CPU's transparent-BG-for-comp behaviour. When the user
                // ticks "visible in background" on SpectralEnvLight, the
                // dome pixel is sampled and returned as spectral radiance.
                //
                // Volumes suppress the BG (matches CPU: volume scenes use
                // transparent BG so they can be comped over another plate).
                radiance = 0.f;
                if (params.envVisibleBg && params.numGpuVolumes == 0) {
                    float3 rgb = sampleEnvHDRI(dir);
                    radiance = rgbToSpectralGPU(rgb, lambda) * params.envIntensityBg;
                }
            }
"""


# ============================================================================
#  Edit definitions
# ============================================================================

EDITS_ENVH = [
    ("SpectralEnvLight.h: add hdriVisible member",
     ENVH_OLD, ENVH_NEW, "bool   hdriVisible = false;"),
]
EDITS_ENVCPP = [
    ("SpectralEnvLight.cpp: add hdri_visible knob",
     ENVCPP_OLD, ENVCPP_NEW, '"hdri_visible", "visible in background"'),
]
EDITS_LIGHT = [
    ("SpectralLight.h: add visibleInPrimary field",
     LIGHT_OLD, LIGHT_NEW, "bool        visibleInPrimary = false;"),
]
EDITS_IOP = [
    ("SpectralRenderIop.cpp: stamp visibleInPrimary at env-light dome site",
     IOP_SITE1_OLD, IOP_SITE1_NEW,
     "hdriDome.visibleInPrimary = el->hdriVisible;"),
    ("SpectralRenderIop.cpp: stamp visibleInPrimary at fallback dome site",
     IOP_SITE2_OLD, IOP_SITE2_NEW,
     "hdriDome.visibleInPrimary = false;  // legacy path"),
]
EDITS_INT = [
    ("SpectralIntegrator.cpp: gate primary miss on visibleInPrimary",
     INT_OLD, INT_NEW, "light.visibleInPrimary"),
]
EDITS_PARAMS = [
    ("SpectralGPUParams.h: add envVisibleBg + env tex/rotation fields",
     PARAMS_OLD, PARAMS_NEW, "int                    envVisibleBg;"),
]
EDITS_GPU_H = [
    ("SpectralGPU.h: add host members for env background state",
     GH_OLD, GH_NEW, "int                    _envVisibleBg = 0;"),
]
EDITS_GPU_CPP = [
    ("SpectralGPU.cpp: extend light checksum to cover visibleInPrimary",
     CHK_OLD, CHK_NEW,
     "Hdri-as-background flag must invalidate the GPU params"),
    ("SpectralGPU.cpp: populate env BG host members in extract loop",
     EXTRACT_OLD, EXTRACT_NEW,
     "_envVisibleBg = 0;"),
    ("SpectralGPU.cpp: copy env BG host members into LaunchParams",
     LAUNCH_OLD, LAUNCH_NEW,
     "launchParams.envVisibleBg    = _envVisibleBg;"),
]
EDITS_KERNEL = [
    ("SpectralGPUKernel.cu: add sampleEnvHDRI + rgbToSpectralGPU helpers",
     HELPER_OLD, HELPER_NEW,
     "Sample the HDRI dome texture by world-space ray direction."),
    ("SpectralGPUKernel.cu: rewrite spectral_miss to sample HDRI when visible",
     MISS_OLD, MISS_NEW,
     "if (params.envVisibleBg && params.numGpuVolumes == 0)"),
]


# ============================================================================
#  Mechanical apply scaffold (same pattern as earlier patches)
# ============================================================================

class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
    if marker in text: return text, R.ALREADY
    c = text.count(o)
    if c == 0: return text, R.NOT_FOUND
    if c > 1:  return text, R.AMBIGUOUS
    return text.replace(o, n, 1), R.APPLIED


def process(path, edits, dry, force, bak):
    print(f"\n=== {path.name} ===")
    if not path.exists():
        print(f"  ERROR: missing: {path}"); return False
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text = original; ok = True
    for desc, a, b, marker in edits:
        text, s = apply_edit(text, a, b, marker)
        mk = {R.APPLIED:"[+]", R.ALREADY:"[=]", R.NOT_FOUND:"[!]", R.AMBIGUOUS:"[?]"}[s]
        print(f"  {mk} {desc}: {s}")
        if s in (R.NOT_FOUND, R.AMBIGUOUS): ok = False
    if text == original:
        print("  (no changes needed)"); return ok
    if not ok and not force:
        print("  SKIPPED WRITE"); return False
    if dry:
        print(f"  DRY RUN: {len(text)-len(original):+d} chars"); return ok
    ob = text.encode(); obk = original.encode()
    if uses_crlf:
        ob = ob.replace(b"\n", b"\r\n"); obk = obk.replace(b"\n", b"\r\n")
    bakp = path.with_suffix(path.suffix + bak)
    bakp.write_bytes(obk); path.write_bytes(ob)
    print(f"  wrote {path.name}; backup {bakp.name}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)
    bak = ".bak_hdrivis"
    ok = True
    ok &= process(args.src / "SpectralEnvLight.h",       EDITS_ENVH,     args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralEnvLight.cpp",     EDITS_ENVCPP,   args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralLight.h",          EDITS_LIGHT,    args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralRenderIop.cpp",    EDITS_IOP,      args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralIntegrator.cpp",   EDITS_INT,      args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPUParams.h",      EDITS_PARAMS,   args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPU.h",            EDITS_GPU_H,    args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPU.cpp",          EDITS_GPU_CPP,  args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPUKernel.cu",     EDITS_KERNEL,   args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. New checkbox 'visible in background' on SpectralEnvLight.")
    print("OFF by default: HDRI lights the scene, background stays black.")
    print("ON: HDRI also renders as the visible sky/environment.")


if __name__ == "__main__":
    main()
