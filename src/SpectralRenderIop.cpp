#include "SpectralRenderIop.h"
#include "SpectralSurfaceOp.h"
#include "SpectralDraftingOp.h"
#include "SpectralShadowCatcherOp.h"
#include "SpectralMeshPropertiesOp.h"
#include <chrono>
#include <cstdlib>

// Debug logging — suppress during scrub for performance
static bool s_spectralLogEnabled = true;
#define SLOG(...) do { if (s_spectralLogEnabled) fprintf(stderr, __VA_ARGS__); } while(0)

#ifdef SPECTRAL_HAS_OSD
#include "SpectralSubdiv.h"
#endif

#ifdef SPECTRAL_HAS_VDB
#include "SpectralVDBLoader.h"
#include "SpectralVDBRead.h"
#include "SpectralVolMerge.h"
#include "SpectralVolumeMaterial.h"
#include "SpectralEnvLight.h"
#include "SpectralStudioLight.h"
#endif

// PXR — USD stage traversal
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdVol/volume.h>
#include <pxr/usd/usdVol/openVDBAsset.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/diagnostic.h>

// USG — Nuke's USD abstraction layer
#include <usg/api.h>
#include <usg/geom/Stage.h>

// DDImage
#include <DDImage/Channel.h>
#include <DDImage/CameraOp.h>
#include <DDImage/Black.h>
#include <DDImage/DeepPlane.h>
#include <DDImage/DeepInfo.h>

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>

PXR_NAMESPACE_USING_DIRECTIVE

// ---------------------------------------------------------------------------
const char* const SpectralRenderIop::CLASS = "SpectralRender";

static Op* SpectralRenderIopCreate(Node* node)
{
    return new SpectralRenderIop(node);
}

const Op::Description SpectralRenderIop::description(
    SpectralRenderIop::CLASS,
    SpectralRenderIopCreate
);

// ---------------------------------------------------------------------------
SpectralRenderIop::SpectralRenderIop(Node* node)
    : Iop(node)
    , _scene(std::make_unique<pxr::SpectralScene>())
{
    // 3 inputs: scene, cam, bg (set by min/max_inputs)
    SLOG("SpectralRender: DLL build %s %s\n", __DATE__, __TIME__);

    // Warm up OptiX in background — PTX compilation takes ~10-15s on first run.
    // Subsequent runs use the disk cache and are instant.
#ifdef SPECTRAL_HAS_OPTIX
    static std::once_flag s_gpuWarmup;
    std::call_once(s_gpuWarmup, []() {
        std::thread([]() {
            SLOG("SpectralRender: warming up GPU (background)...\n");
            auto t0 = std::chrono::high_resolution_clock::now();
            SpectralIntegrator::IsGPUAvailable();
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();
            SLOG("SpectralRender: GPU warmup complete (%lldms)\n", ms);
        }).detach();
    });
#endif
}

SpectralRenderIop::~SpectralRenderIop() {
    _asyncCancel.store(true);
    if (_asyncQualityThread.joinable()) _asyncQualityThread.join();
}

// Pre-populated grid menu — common VDB grid names (shared with SpectralVDBRead)
const char* const SpectralRenderIop::kVdbGridMenu[] = {
    "(none)", "density", "density_1", "smoke", "soot",
    "temperature", "temp", "heat",
    "flame", "fire", "fuel", "burn",
    "vel", "velocity", "v", "motion",
    "Cd", "color", "colour", "albedo",
    nullptr
};

const char* SpectralRenderIop::_VdbGridName(int idx, const char* ovr) const
{
    if (ovr && strlen(ovr) > 0) return ovr;
    if (idx > 0) return kVdbGridMenu[idx];
    return nullptr;
}

int SpectralRenderIop::_GetMasterMaxRes() const
{
    if (_vdbVolRes == 4) return 1024;
    if (_vdbVolRes == 3) return 512;
    if (_vdbVolRes == 2) return 256;
    if (_vdbVolRes == 1) return 128;
    return 64;
}

const char* SpectralRenderIop::node_help() const
{
    return
        "SpectralRender — physically-based spectral path tracer.\n\n"
        "Input 0 (Cam): Connect a Camera node (optional, defaults to 24mm).\n"
        "Volumes and materials are auto-detected from upstream nodes.\n\n"
        "Input 1 (Scene): Connect GeoScene with geometry and/or SpectralVDBRead.\n"
        "Volumes and materials are auto-detected from upstream nodes.\n"
        "or a default 50mm perspective.\n\n"
        "Input 2 (BG): Connect any 2D image. Its format sets the\n"
        "output resolution. The BG image is rendered behind the scene.\n\n"
        "Created by Marten Blumen\n"
        "github.com/bratgot/SpectralRenderer";}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
const char* SpectralRenderIop::input_label(int idx, char*) const
{
    switch (idx) {
        case 0: return "cam";
        case 1: return "scn";
        case 2: return "bg";
        default: return nullptr;
    }
}

bool SpectralRenderIop::test_input(int idx, Op* op) const
{
    if (!op) return true;
    switch (idx) {
        case 0: return dynamic_cast<CameraOp*>(op) != nullptr;
        case 1: return dynamic_cast<GeometryProviderI*>(op) != nullptr;
        case 2: return dynamic_cast<Iop*>(op) != nullptr;
        default: return false;
    }
}

Op* SpectralRenderIop::default_input(int idx) const
{
    if (idx == 2) return Iop::default_input(0);
    return nullptr;  // scene, cam, disp are optional
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------
void SpectralRenderIop::knobs(Knob_Callback f)
{
    Text_knob(f, "<b><font size='+1'>SpectralRenderer</font></b><br>"
                 "<font color='#888'>Physically-based spectral path tracer</font>");

    String_knob(f, &_labelStr, "label", "");
    SetFlags(f, Knob::INVISIBLE);

    // Scene section hidden — USD file and camera prim managed elsewhere
    File_knob(f, &_usdFilePath, "usd_file", "USD file"); SetFlags(f, Knob::INVISIBLE);
    String_knob(f, &_cameraPath, "camera_path", "camera prim"); SetFlags(f, Knob::INVISIBLE);
    Format_knob(f, &_outputFormat, "format", "format");

    // ─── Compatibility ──────────────────────────────────────────────
    Divider(f, "Compatibility");
    Bool_knob(f, &_scanlineCompat, "scanline_compat", "ScanlineRender compatible");
    Tooltip(f, "Emulate ScanlineRender behaviours for drop-in replacement.\n\n"
               "When enabled:\n"
               "\xe2\x80\xa2 Smooth vertex normals (auto-averaged, not faceted)\n"
               "\xe2\x80\xa2 Alpha cutout (transparent texture regions produce no shading)\n"
               "\xe2\x80\xa2 Premultiplied alpha (colour \xc3\x97 opacity, ready for Over comp)\n"
               "\xe2\x80\xa2 Black background (no dome colour in background)\n"
               "\xe2\x80\xa2 Constant shader when no lights (flat material colour)\n\n"
               "Disable for physically-based spectral rendering with face normals.");
    Bool_knob(f, &_neutralBalance, "neutral_balance", "neutral white balance");
    Tooltip(f, "Prevent spectral colour shift on textures.\n\n"
               "The spectral rendering pipeline converts RGB to wavelength-dependent\n"
               "reflectance and back. This round-trip can introduce a subtle colour\n"
               "tint that makes white textures appear slightly warm or cool.\n\n"
               "When enabled, a correction factor is applied so that a white texture\n"
               "under white light produces pure white output.\n\n"
               "NOTE: Only applies when 'ScanlineRender compatible' is OFF.\n"
               "In compatible mode, rendering bypasses the spectral pipeline\n"
               "entirely so there is no colour shift to correct.\n\n"
               "Leave on for spectral rendering. Has no effect in compatible mode.");
    Text_knob(f,
        "<font color='#666' size='-1'>"
        "Matches ScanlineRender output for geometry cards, textured planes,<br>"
        "and simple lit scenes. Works with ReadGeo \xe2\x86\x92 GeoScene \xe2\x86\x92 SpectralRender<br>"
        "without needing a SpectralSurface material."
        "</font>"
    );

    // ─── Re-render ──────────────────────────────────────────────────
    Divider(f, "");
    Button(f, "rerender", "<font color='#c44'>Re-render</font>");
    Tooltip(f, "Force a complete re-render from scratch.\n\n"
               "This will:\n"
               "\xe2\x80\xa2 Flush the entire scene cache\n"
               "\xe2\x80\xa2 Re-read all input textures and geometry\n"
               "\xe2\x80\xa2 Rebuild the GPU acceleration structure\n"
               "\xe2\x80\xa2 Clear all material and light caches\n\n"
               "Use when upstream changes are not detected automatically,\n"
               "or after connecting/disconnecting nodes in the tree.");
    Text_knob(f,
        "<font color='#666' size='-1'>"
        "Flushes all caches and forces a full re-read of the node graph.<br>"
        "Use if the render appears stale after upstream edits."
        "</font>"
    );

    // ─── Technical Notes ────────────────────────────────────────────
    BeginClosedGroup(f, "tech_notes_grp", "Technical Notes");
    {
        Text_knob(f,
            "<b><font size='+1'>SpectralRenderer</font></b><br>"
            "<font color='#999'>Physically-based spectral path tracer for Nuke</font><br>"
            "<br>"

            "<b>Overview</b><br>"
            "SpectralRenderer is a full spectral path tracer that simulates light transport<br>"
            "at individual wavelengths across the visible spectrum (380\xe2\x80\x93" "780 nm). Unlike<br>"
            "conventional RGB renderers that compute three colour channels independently,<br>"
            "SpectralRenderer samples one wavelength per ray and accumulates radiance<br>"
            "through CIE 1931 colour matching functions, producing physically accurate<br>"
            "colour reproduction including dispersion, thin-film interference, fluorescence,<br>"
            "and wavelength-dependent absorption.<br>"
            "<br>"

            "<b>Spectral Integration</b><br>"
            "For each pixel, <i>N</i> samples are traced at wavelengths \xce\xbb drawn uniformly<br>"
            "from [380, 780] nm. The spectral radiance <i>L</i>(\xce\xbb) at each hit is converted<br>"
            "to CIE XYZ tristimulus values:<br>"
            "<br>"
            "<font face='monospace'>"
            "&nbsp;&nbsp;X = (1/N) \xce\xa3 L(\xce\xbb\xe1\xb5\xa2) \xc2\xb7 x\xcc\x84(\xce\xbb\xe1\xb5\xa2) \xc2\xb7 \xce\x94\xce\xbb<br>"
            "&nbsp;&nbsp;Y = (1/N) \xce\xa3 L(\xce\xbb\xe1\xb5\xa2) \xc2\xb7 y\xcc\x84(\xce\xbb\xe1\xb5\xa2) \xc2\xb7 \xce\x94\xce\xbb<br>"
            "&nbsp;&nbsp;Z = (1/N) \xce\xa3 L(\xce\xbb\xe1\xb5\xa2) \xc2\xb7 z\xcc\x84(\xce\xbb\xe1\xb5\xa2) \xc2\xb7 \xce\x94\xce\xbb<br>"
            "</font>"
            "<br>"
            "where x\xcc\x84, y\xcc\x84, z\xcc\x84 are the CIE 1931 colour matching functions and<br>"
            "\xce\x94\xce\xbb = 400/106.857 is the normalisation factor. XYZ is then converted to<br>"
            "the output colour space (sRGB, ACEScg, or ACES 2065-1).<br>"
            "<br>"

            "<b>Material Model</b><br>"
            "Surfaces use a Disney/GGX BSDF with spectral extensions:<br>"
            "<br>"
            "<font face='monospace'>"
            "&nbsp;&nbsp;f(\xce\xbb) = (1\xe2\x88\x92m) \xc2\xb7 f<sub>diff</sub>(\xce\xbb) + m \xc2\xb7 f<sub>spec</sub>(\xce\xbb)<br>"
            "&nbsp;&nbsp;f<sub>spec</sub> = D(h,\xce\xb1) \xc2\xb7 G(v,l,\xce\xb1) \xc2\xb7 F(\xce\xbb,\xce\xb8) / (4 \xc2\xb7 N\xc2\xb7V \xc2\xb7 N\xc2\xb7L)<br>"
            "</font>"
            "<br>"
            "where <i>m</i> = metallic, <i>D</i> = GGX normal distribution, <i>G</i> = Smith geometry<br>"
            "term, <i>F</i> = wavelength-dependent Fresnel, and \xce\xb1 = roughness\xc2\xb2.<br>"
            "<br>"
            "Material colour is converted from RGB to a spectral reflectance curve<br>"
            "using three Gaussian basis functions centred at the CIE primaries:<br>"
            "<br>"
            "<font face='monospace'>"
            "&nbsp;&nbsp;R(\xce\xbb) = r \xc2\xb7 G(\xce\xbb, 630, 30) + g \xc2\xb7 G(\xce\xbb, 532, 30) + b \xc2\xb7 G(\xce\xbb, 460, 25)<br>"
            "</font>"
            "<br>"

            "<b>Spectral Phenomena</b><br>"
            "\xe2\x80\xa2 <b>Dispersion</b> \xe2\x80\x94 Wavelength-dependent IOR via the Cauchy equation:<br>"
            "<font face='monospace'>"
            "&nbsp;&nbsp;&nbsp;&nbsp;n(\xce\xbb) = n<sub>d</sub> + (n<sub>d</sub> \xe2\x88\x92 1) / V<sub>d</sub> \xc2\xb7 (C / \xce\xbb\xc2\xb2 \xe2\x88\x92 1)<br>"
            "</font>"
            "&nbsp;&nbsp;&nbsp;&nbsp;where V<sub>d</sub> is the Abbe number. Produces rainbow caustics in glass.<br>"
            "<br>"
            "\xe2\x80\xa2 <b>Thin-Film Interference</b> \xe2\x80\x94 Iridescent reflections from nanoscale coatings:<br>"
            "<font face='monospace'>"
            "&nbsp;&nbsp;&nbsp;&nbsp;\xce\xb4 = 2\xc2\xb7n<sub>film</sub>\xc2\xb7" "d\xc2\xb7" "cos(\xce\xb8<sub>t</sub>) / \xce\xbb<br>"
            "&nbsp;&nbsp;&nbsp;&nbsp;R = R<sub>0</sub> + (1 \xe2\x88\x92 R<sub>0</sub>) \xc2\xb7 sin\xc2\xb2(\xcf\x80 \xc2\xb7 \xce\xb4)<br>"
            "</font>"
            "&nbsp;&nbsp;&nbsp;&nbsp;Produces oil-slick and soap-bubble colour shifts.<br>"
            "<br>"
            "\xe2\x80\xa2 <b>Fluorescence</b> \xe2\x80\x94 Absorbs at one wavelength, re-emits at another.<br>"
            "\xe2\x80\xa2 <b>Diffraction Gratings</b> \xe2\x80\x94 Wavelength-dependent angular splitting.<br>"
            "\xe2\x80\xa2 <b>Volume Absorption</b> \xe2\x80\x94 Beer-Lambert extinction through coloured media.<br>"
            "<br>"

            "<b>GPU Acceleration</b><br>"
            "The GPU path uses NVIDIA OptiX 9 with RTX hardware ray tracing and<br>"
            "Shader Execution Reordering (SER) for optimal warp coherence on<br>"
            "Ada Lovelace and Blackwell architectures. CUDA 12.6 kernels handle<br>"
            "spectral shading, volume ray marching, and NanoVDB sampling.<br>"
            "<br>"
            "The CPU path uses Intel Embree 4 with a BVH acceleration structure<br>"
            "and std::execution::par_unseq for automatic SIMD parallelism.<br>"
            "<br>"

            "<b>Volume Rendering</b><br>"
            "OpenVDB volumes are rendered with ray marching through density,<br>"
            "temperature, and flame fields. The volume integrator supports:<br>"
            "\xe2\x80\xa2 Single-scattering with Henyey-Greenstein phase function<br>"
            "\xe2\x80\xa2 Blackbody emission from temperature grids (Planck\xe2\x80\x99s law)<br>"
            "\xe2\x80\xa2 Multi-scattering approximation for dense clouds<br>"
            "\xe2\x80\xa2 NanoVDB LOD streaming for out-of-core datasets<br>"
            "<br>"

            "<b>ScanlineRender Drop-In</b><br>"
            "With the \xe2\x80\x98ScanlineRender compatible\xe2\x80\x99 checkbox enabled, SpectralRenderer<br>"
            "can replace Nuke\xe2\x80\x99s built-in ScanlineRender in existing comp trees:<br>"
            "<br>"
            "\xe2\x80\xa2 Reads native Nuke materials (BasicMaterial, Phong, NukeDefaultSurface)<br>"
            "\xe2\x80\xa2 Supports MtlXStandardSurface, PreviewSurface, and ReflectiveSurface<br>"
            "\xe2\x80\xa2 Auto-smooth normals eliminate faceted shading on simple geometry<br>"
            "\xe2\x80\xa2 Texture alpha is respected for transparency cutouts on cards<br>"
            "\xe2\x80\xa2 Premultiplied output comps directly over plates with Over nodes<br>"
            "\xe2\x80\xa2 No-light scenes render as constant flat colour (same as ScanlineRender)<br>"
            "\xe2\x80\xa2 Works with ReadGeo \xe2\x86\x92 GeoScene \xe2\x86\x92 SpectralRender without SpectralSurface<br>"
            "<br>"
            "This means artists can upgrade from ScanlineRender to SpectralRenderer<br>"
            "and immediately gain soft shadows, global illumination, spectral dispersion,<br>"
            "and physically-based volume rendering \xe2\x80\x94 without re-shading any geometry.<br>"
            "<br>"

            "<b>For Artists</b><br>"
            "SpectralRenderer is designed to produce results that are both physically<br>"
            "correct and artistically controllable. Key strengths:<br>"
            "<br>"
            "\xe2\x80\xa2 <b>True glass and diamonds</b> \xe2\x80\x94 rainbow dispersion happens naturally,<br>"
            "&nbsp;&nbsp;controlled by a single Abbe number on the material.<br>"
            "<br>"
            "\xe2\x80\xa2 <b>Iridescence without maps</b> \xe2\x80\x94 thin-film interference creates oil-slick<br>"
            "&nbsp;&nbsp;and soap-bubble colours from a single thickness value.<br>"
            "<br>"
            "\xe2\x80\xa2 <b>Accurate fire and explosions</b> \xe2\x80\x94 VDB temperature grids emit blackbody<br>"
            "&nbsp;&nbsp;colour automatically. No hand-painted colour ramps needed.<br>"
            "<br>"
            "\xe2\x80\xa2 <b>GPU speed</b> \xe2\x80\x94 RTX hardware acceleration renders complex scenes<br>"
            "&nbsp;&nbsp;in milliseconds. Interactive preview in the 3D viewport.<br>"
            "<br>"
            "\xe2\x80\xa2 <b>Production AOVs</b> \xe2\x80\x94 diffuse/specular direct+indirect, emission,<br>"
            "&nbsp;&nbsp;transmission, normals, position, UV, depth, object/material ID,<br>"
            "&nbsp;&nbsp;and Cryptomatte for compositing flexibility.<br>"
            "<br>"
            "\xe2\x80\xa2 <b>Drop-in replacement</b> \xe2\x80\x94 ScanlineRender compatible mode means you<br>"
            "&nbsp;&nbsp;can swap renderers without changing your comp tree."
        );
    }
    EndGroup(f);

    // ─── Credit ─────────────────────────────────────────────────────
    Divider(f, "");
    Text_knob(f,
        "<font color='#555' size='-1'>"
        "Created by Marten Blumen"
        "</font>"
    );

    BeginGroup(f, "render_grp", "Render");
    {
        static const char* const deviceNames[] = { "cpu", "gpu", "auto", nullptr };
        Enumeration_knob(f, &_deviceMode, deviceNames, "device_mode", "device mode");
        Tooltip(f, "Rendering device:<br>"
                   "cpu = Embree 4 CPU ray tracing<br>"
                   "gpu = OptiX 8.1 RTX GPU ray tracing<br>"
                   "auto = GPU if available, otherwise CPU");
        static const char* const projNames[] = { "perspective", "UV", "spherical", nullptr };
        Enumeration_knob(f, &_projectionMode, projNames, "projection", "projection");
        Tooltip(f, "Camera projection mode.\n\n"
                   "perspective = standard camera (default)\n"
                   "UV = renders geometry flat-unwrapped in UV space.\n"
                   "    Output pixels map directly to texture coordinates.\n"
                   "    Used for baking textures, painting patches,\n"
                   "    and creating UV-space AOVs for compositing.\n\n"
                   "spherical = latitude/longitude equirectangular projection.\n"
                   "    Each pixel maps to a direction on the sphere.\n"
                   "    Used for environment map rendering and 360 output.");
        static const char* const csNames[] = { "sRGB", "ACEScg", "ACES 2065-1", nullptr };
        Enumeration_knob(f, &_colorSpace, csNames, "color_space", "color space");
        Tooltip(f, "Output color space for spectral-to-RGB conversion.<br>"
                   "sRGB = standard (Rec.709 primaries, D65)<br>"
                   "ACEScg = ACES working space (AP1, D60)<br>"
                   "ACES 2065-1 = ACES interchange (AP0, D60)<br>"
                   "ACEScg recommended for compositing pipelines.");
        static const char* const proxyNames[] = { "1/4", "1/2", "3/4", "full", nullptr };
        Enumeration_knob(f, &_proxyMode, proxyNames, "proxy", "proxy");
        Tooltip(f, "Render resolution proxy.<br>"
                   "1/4 = quarter resolution (fastest preview)<br>"
                   "1/2 = half resolution<br>"
                   "3/4 = three-quarter resolution<br>"
                   "full = full output resolution");
        Int_knob(f, &_samples, "spp", "shading samples"); SetRange(f, 1, 256);
        Tooltip(f, "Number of spectral samples per pixel for shading.<br>"
                   "Higher values reduce noise. Each sample<br>"
                   "traces one wavelength through the scene.<br>"
                   "1 = single sample, 16+ = clean spectral.");
        Int_knob(f, &_edgeSamples, "edge_samples", "edge samples"); SetRange(f, 0, 16);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Additional samples along geometry edges to reduce aliasing.<br>"
                   "0 = disabled (fastest)<br>"
                   "2-4 = subtle smoothing<br>"
                   "8-16 = clean anti-aliased edges<br><br>"
                   "Only edge pixels are supersampled (detected via depth/<br>"
                   "object ID discontinuities). Interior pixels are not affected.<br>"
                   "This is much cheaper than raising shading samples.");
        Int_knob(f, &_volumeSpp, "vol_spp", "vol samples"); SetRange(f, 1, 256);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Samples per pixel for volume rendering.<br>"
                   "Volumes need fewer samples than geometry (4-8 typical).<br>"
                   "0 = use geometry spp value.");
        Int_knob(f, &_maxBounces, "max_bounces", "max bounces"); SetRange(f, 1, 16);
        Tooltip(f, "Maximum ray bounce depth for all paths.<br>"
                   "1 = direct lighting only<br>"
                   "4 = good for most scenes (default)<br>"
                   "8+ = complex glass and caustics");
        Int_knob(f, &_refractionBounces, "refraction_bounces", "refraction bounces"); SetRange(f, 1, 32);
        Tooltip(f, "Maximum bounces for transmission/refraction paths.<br>"
                   "Glass needs at least 4 (enter+exit+reflection).<br>"
                   "Nested glass (ice in water) needs 8+.<br>"
                   "Default 8. Does not affect diffuse/specular depth.");
        Int_knob(f, &_tileSize, "tile_size", "tile size"); SetRange(f, 16, 256);
        Tooltip(f, "Tile size in pixels for parallel rendering.<br>"
                   "Smaller tiles = better load balancing.<br>"
                   "Larger tiles = less overhead. Default 64.");
        Float_knob(f, &_adaptiveThreshold, "adaptive_threshold", "adaptive threshold");
        SetRange(f, 0.f, 0.5f);
        Tooltip(f, "Adaptive sampling convergence threshold.<br>"
                   "Pixels with noise below this stop receiving<br>"
                   "samples, saving render time on clean areas.<br>"
                   "0.0 = disabled, 0.05 = default, 0.1 = aggressive");
        Newline(f);
        Bool_knob(f, &_progressive, "progressive", "progressive");
        Tooltip(f, "Progressive refinement mode. First render<br>"
                   "produces a fast 8-spp preview. Subsequent<br>"
                   "renders accumulate to full sample count.<br>"
                   "Any parameter change resets the preview.");
        Bool_knob(f, &_blueNoise, "blue_noise", "blue noise");
        Tooltip(f, "R2 quasi-random sampling sequence for<br>"
                   "better sample distribution. Reduces visible<br>"
                   "noise patterns at low sample counts compared<br>"
                   "to pure random sampling.");
        Bool_knob(f, &_denoise, "denoise", "denoise");
        Tooltip(f, "Apply OptiX AI denoiser as a post-process.<br>"
                   "Dramatically cleans up noisy low-spp renders.<br>"
                   "Works on both CPU and GPU rendered images.<br>"
                   "Requires an NVIDIA GPU with OptiX support.");
        Divider(f, "Depth of field");
        Float_knob(f, &_fStop, "fstop", "f-stop");
        SetRange(f, 0.f, 22.f);
        Tooltip(f, "Lens aperture for depth of field.<br>"
                   "0 = pinhole camera, no DOF (default)<br>"
                   "1.4 = very shallow DOF<br>"
                   "5.6 = moderate DOF<br>"
                   "16+ = nearly everything in focus");
        Float_knob(f, &_focusDistance, "focus_distance", "focus distance");
        SetRange(f, 0.1f, 10000.f);
        Tooltip(f, "Distance from camera to the focal plane<br>"
                   "in world units. Objects at this distance<br>"
                   "will be sharp; nearer and farther objects blur.");
    }
    EndGroup(f);

    BeginGroup(f, "lighting_grp", "Lighting");
    {
        Bool_knob(f, &_useBuiltinLight, "use_builtin_light", "built-in light");
        Tooltip(f, "Enable the built-in sun/sky light.\n"
                   "Uses the sky preset and sun direction from the hidden Lighting tab.\n"
                   "Automatically disabled when SpectralEnvLight is connected.\n"
                   "Disable to use only scene graph lights (GeoDiskLight etc).");
        Float_knob(f, &_lightIntensity, "light_intensity", "intensity multiplier");
        SetRange(f, 0.01f, 10.f);
        Tooltip(f, "Global multiplier applied to all light intensities.<br>"
                   "Nuke's USD pipeline uses physical light units<br>"
                   "which can produce very bright values. Adjust<br>"
                   "this to compensate. Default 1.0.");
        static const char* const illuminantNames[] = {
            "auto", "D50", "D65", "A", "F2", "F11", nullptr
        };
        Enumeration_knob(f, &_illuminant, illuminantNames, "illuminant", "illuminant");
        Tooltip(f, "CIE standard illuminant override for all lights.<br>"
                   "auto = use each light's own colour or temperature<br>"
                   "D50 = horizon daylight (5003K)<br>"
                   "D65 = noon daylight (6504K)<br>"
                   "A = tungsten incandescent (2856K)<br>"
                   "F2 = cool white fluorescent<br>"
                   "F11 = narrow-band fluorescent");
    }
    EndGroup(f);

    Tab_knob(f, "Lighting"); SetFlags(f, Knob::INVISIBLE);
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Analytical sun/sky lighting. Composes additively with scene lights<br>"
            "from the scn input \xe2\x80\x94 Direct, Sphere, and Dome lights all contribute."
            "</font>"
        );

        // ─── Sky Preset ─────────────────────────────────────────────
        static const char* const skyP[] = {
            "Off", "Custom",
            "Clear Day", "Golden Hour", "Red Sky Dawn", "Sunrise",
            "Overcast", "Blue Hour", "Moonlit", "Starlight",
            "Alpine Light", "Desert Noon", "Arctic Twilight",
            "Mars", "Titan", "Krypton", "Tatooine", "Pandora",
            nullptr
        };
        Enumeration_knob(f, &_skyPreset, skyP, "sky_preset", "Sky Preset");
        Tooltip(f, "Analytical sun/sky presets.\n\n"
                   "Earth atmospheres:\n"
                   "  Clear Day \xe2\x80\x94 bright noon, low turbidity\n"
                   "  Golden Hour \xe2\x80\x94 warm low sun, long shadows\n"
                   "  Red Sky Dawn \xe2\x80\x94 deep red/orange pre-sunrise\n"
                   "  Sunrise \xe2\x80\x94 first light, warm golds and pinks\n"
                   "  Overcast \xe2\x80\x94 soft diffuse, no direct sun\n"
                   "  Blue Hour \xe2\x80\x94 cool twilight, 2\xc2\xb0 elevation\n"
                   "  Moonlit \xe2\x80\x94 silver-blue moonlight\n"
                   "  Starlight \xe2\x80\x94 deep night, minimal ambient\n"
                   "  Alpine Light \xe2\x80\x94 high altitude, UV-rich, clear\n"
                   "  Desert Noon \xe2\x80\x94 harsh overhead, high turbidity\n"
                   "  Arctic Twilight \xe2\x80\x94 perpetual low sun\n\n"
                   "Other worlds:\n"
                   "  Mars \xe2\x80\x94 butterscotch sky, cold thin atmosphere\n"
                   "  Titan \xe2\x80\x94 orange methane haze\n"
                   "  Krypton \xe2\x80\x94 red giant star, deep crimson sky\n"
                   "  Tatooine \xe2\x80\x94 binary sunset, twin warm suns\n"
                   "  Pandora \xe2\x80\x94 bioluminescent blue-violet");
        Double_knob(f, &_skyMix, "sky_mix", "sky mix"); SetRange(f, 0, 2);
        Tooltip(f, "Blend between sky model and scene lights.\n"
                   "0 = scene lights only. 1 = full sky. >1 = boost.");

        // ─── Location + Time ────────────────────────────────────────
        BeginClosedGroup(f, "grp_location", "Location and time of day");
        {
            Text_knob(f,
                "<font color='#777' size='-1'>"
                "Calculate sun position from geographic location and time.<br>"
                "Uses the solar position algorithm (SPA) for accurate elevation/azimuth.<br>"
                "Clicking 'Apply' sets sun elevation and azimuth from these values."
                "</font>"
            );
            Newline(f);
            static const char* const locP[] = {
                "Custom", "London", "New York", "Los Angeles", "Tokyo", "Sydney",
                "Paris", "Dubai", "Reykjavik", "Cape Town", "Mumbai",
                "North Pole", "Equator (Quito)", nullptr
            };
            Enumeration_knob(f, &_locationPreset, locP, "location_preset", "location");
            Tooltip(f, "Named locations set latitude and longitude.\n"
                       "Choose Custom to enter coordinates manually.");
            Double_knob(f, &_latitude, "latitude", "latitude"); SetRange(f, -90, 90);
            Tooltip(f, "Geographic latitude. +90 = North Pole. -90 = South Pole.");
            Double_knob(f, &_longitude, "longitude", "longitude"); SetRange(f, -180, 180);
            ClearFlags(f, Knob::STARTLINE);
            Double_knob(f, &_timeOfDay, "time_of_day", "time (24h)"); SetRange(f, 0, 24);
            Tooltip(f, "Local solar time. 6 = sunrise, 12 = noon, 18 = sunset.\n"
                       "Fractional hours: 14.5 = 2:30 PM.");
            Int_knob(f, &_dayOfYear, "day_of_year", "day of year"); SetRange(f, 1, 365);
            ClearFlags(f, Knob::STARTLINE);
            Tooltip(f, "1 = Jan 1. 80 = March equinox. 172 = June solstice.\n"
                       "264 = Sep equinox. 355 = Dec solstice.");
            Button(f, "apply_sun_position", "Apply Sun Position");
            Tooltip(f, "Calculate sun elevation and azimuth from location,\n"
                       "time, and day. Sets sun_elevation and sun_azimuth knobs.");
        }
        EndGroup(f);

        // ─── Sun and Sky ────────────────────────────────────────────
        BeginClosedGroup(f, "grp_sun", "Sun and sky settings");
        {
            Double_knob(f, &_sunElevation, "sun_elevation", "sun elevation"); SetRange(f, -10, 90);
            Tooltip(f, "Sun angle above horizon in degrees.\n"
                       "-5 to 0 = below horizon (twilight/dawn colours)\n"
                       "0 to 10 = golden/red hour\n"
                       "10 to 30 = morning/afternoon\n"
                       "60 to 90 = near noon");
            Double_knob(f, &_sunAzimuth, "sun_azimuth", "sun azimuth"); SetRange(f, 0, 360);
            Tooltip(f, "Sun compass direction. 0/360 = North. 90 = East.\n"
                       "180 = South. 270 = West.");
            Double_knob(f, &_sunIntensity, "sun_intensity", "sun intensity"); SetRange(f, 0, 100);
            SetFlags(f, Knob::LOG_SLIDER);
            Tooltip(f, "Direct sun brightness (squared for exponential feel).\n"
                       "Effective = value^2. So 3->9, 5->25, 10->100.\n"
                       "1 = dim fill. 5 = overcast. 8 = bright sun. 15+ = intense.");
            Double_knob(f, &_skyIntensity, "sky_intensity", "sky fill"); SetRange(f, 0, 5);
            SetFlags(f, Knob::LOG_SLIDER);
            Tooltip(f, "Sky dome fill brightness (squared).\n"
                       "Effective = value^2. So 2->4, 5->25.\n"
                       "1 = subtle. 3 = natural. 5+ = bright overcast.");
            Double_knob(f, &_turbidity, "turbidity", "turbidity"); SetRange(f, 2, 10);
            Tooltip(f, "Atmospheric haze / particulates.\n"
                       "2 = crystal clear mountain air\n"
                       "3 = clear day (default)\n"
                       "5 = light haze / coastal\n"
                       "8 = heavy haze / smog\n"
                       "10 = dense urban pollution");
        }
        EndGroup(f);

        Divider(f, "Studio lights");
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Three-point studio rig: key (main), fill (shadow lift), rim (edge separation).<br>"
            "Composes additively with the sun/sky model above."
            "</font>"
        );
        static const char* const stuP[] = {"Off", "Portrait", "Product", "Dramatic", "Softbox", nullptr};
        Enumeration_knob(f, &_studioPreset, stuP, "studio_preset", "Studio Preset");
        Tooltip(f, "Portrait -- soft key 45deg, moderate fill, subtle rim.\n"
                   "  Best for: character lighting, close-ups.\n"
                   "Product -- bright key 60deg, low fill, strong rim.\n"
                   "  Best for: product shots, hard surfaces.\n"
                   "Dramatic -- steep key 80deg, near-zero fill, intense rim.\n"
                   "  Best for: moody scenes, noir look.\n"
                   "Softbox -- broad key 30deg, high fill, gentle rim.\n"
                   "  Best for: beauty, soft wrapping light.");
        Double_knob(f, &_studioMix, "studio_mix", "studio mix"); SetRange(f, 0, 2);

        BeginClosedGroup(f, "grp_studio", "Studio light settings");
        {
            Double_knob(f, &_studioKeyAzimuth, "studio_key_azimuth", "key azimuth"); SetRange(f, 0, 360);
            Double_knob(f, &_studioKeyElevation, "studio_key_elevation", "key elevation"); SetRange(f, 0, 90);
            Double_knob(f, &_studioKeyIntensity, "studio_key_intensity", "key intensity"); SetRange(f, 0, 20);
            Double_knob(f, &_studioFillRatio, "studio_fill_ratio", "fill ratio"); SetRange(f, 0, 1);
            Tooltip(f, "Fill brightness relative to key. 0=no fill, 1=flat.");
            Double_knob(f, &_studioRimIntensity, "studio_rim_intensity", "rim intensity"); SetRange(f, 0, 20);
        }
        EndGroup(f);

        Divider(f, "");
        Double_knob(f, &_shadowSoftness, "shadow_softness", "shadow softness"); SetRange(f, 0, 1);
        Tooltip(f, "Controls shadow edge quality from built-in lights.\n"
                   "Simulates area light sources (larger = softer shadows).\n"
                   "0 = razor sharp (point light)\n"
                   "0.2 = slightly soft (small area)\n"
                   "0.5 = medium soft (overcast feel)\n"
                   "1.0 = very diffuse (large softbox)");

        // ─── Environment Map ────────────────────────────────────────────
        Divider(f, "Environment map");
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "HDRI environment maps provide image-based lighting from all directions.<br>"
            "Load an .hdr or .exr file below, or connect an EnvironLight to scn input."
            "</font>"
        );
        Newline(f);
        File_knob(f, &_hdriFile, "hdri_file", "HDRI file");
        Tooltip(f, "Load an HDRI environment map (.hdr, .exr, .tx).\n"
                   "Creates a dome light that illuminates both geometry and volumes.\n"
                   "This overrides any EnvironLight connected to the scn input.");
        Double_knob(f, &_hdriIntensity, "hdri_intensity", "HDRI intensity"); SetRange(f, 0, 20);
        SetFlags(f, Knob::LOG_SLIDER);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Brightness multiplier for the HDRI dome light.\n"
                   "1 = as-authored. 5 = boosted. 10+ = very bright.");
        Double_knob(f, &_hdriRotate, "hdri_rotate", "HDRI rotate");
        SetRange(f, 0, 360);
        Tooltip(f, "Rotate HDRI horizontally in degrees.\n"
                   "Repositions the sun/sky without re-rendering the map.");
        Divider(f, "");
        Double_knob(f, &_vdbEnvIntensity, "vdb_env_intensity", "env intensity"); SetRange(f, 0, 10);
        SetFlags(f, Knob::LOG_SLIDER);
        Tooltip(f, "Environment map brightness multiplier.\n"
                   "Logarithmic slider. 0.5 = dim. 1 = as-authored. 3+ = bright.");
        Double_knob(f, &_vdbEnvRotate, "vdb_env_rotate", "env rotate");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 360);
        Tooltip(f, "Rotate the environment map horizontally in degrees.\n"
                   "Useful to reposition the sun in an HDRI without re-rendering.");
        Double_knob(f, &_vdbEnvDiffuse, "vdb_env_diffuse", "env diffuse"); SetRange(f, 0, 2);
        Tooltip(f, "Environment contribution to diffuse illumination.\n"
                   "0 = off. 0.5 = natural. 1 = full. >1 = boosted.\n"
                   "Affects both volume scatter and surface diffuse lighting.");
        Divider(f, "");
        static const char* const envModes[] = {
            "Average colour (fast)", "SH + Virtual Lights", nullptr
        };
        Enumeration_knob(f, &_vdbEnvMode, envModes, "vdb_env_mode", "env mode");
        Tooltip(f, "Average colour: uses mean HDRI colour for isotropic ambient.\n"
                   "SH + Virtual Lights: 9 SH coefficients + brightest peaks\n"
                   "extracted as virtual directional lights with shadow rays.");
        Int_knob(f, &_vdbEnvVirtualLights, "vdb_env_virtual_lights", "virtual lights"); SetRange(f, 0, 4);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "HDRI peaks extracted as virtual directional lights.\n"
                   "0 = ambient only. 2 = recommended. 4 = max.");
        Bool_knob(f, &_vdbUseReSTIR, "vdb_use_restir", "ReSTIR env sampling");
        Tooltip(f, "Importance sampling for env lighting. ~26x fewer shadow rays.\n"
                   "Currently stubbed -- full implementation in Phase 13.");
    }


    BeginClosedGroup(f, "spectral_engine", "Technical");
    {
        Text_knob(f,
            "<font size='-1' color='#999'>"
            "<b>Rendering</b><br>"
            "Hero-wavelength spectral path tracer. Each ray samples one wavelength<br>"
            "(380-780nm), accumulates CIE XYZ tristimulus values via Monte Carlo.<br>"
            "Disney Principled BSDF [Burley 2012] with GGX microfacet specular.<br>"
            "Embree 4 (CPU) + OptiX 8.1 RTX (GPU). Blue noise R2 sampling.<br>"
            "<br>"
            "<b>Spectral effects (GPU + CPU)</b><br>"
            "Cauchy dispersion via Abbe number. Thin-film interference (Fabry-Perot).<br>"
            "Diffraction gratings (3 orders). Fluorescence (Stokes shift).<br>"
            "Conductor Fresnel from measured (n,k) data [Palik].<br>"
            "Kulla-Conty multi-scatter GGX energy compensation.<br>"
            "Beer-Lambert volumetric absorption per wavelength.<br>"
            "Subsurface scattering: spectral random walk (CPU).<br>"
            "<br>"
            "<b>Volumes (GPU + CPU)</b><br>"
            "OpenVDB. Up to 8 volumes composited on GPU. Per-volume materials.<br>"
            "Cornette-Shanks Mie phase [Jendersie, d'Eon 2023].<br>"
            "Multiple scattering [Wrenninge 2015]. Chromatic extinction.<br>"
            "Powder effect [Schneider, Vos 2015]. fBm procedural noise.<br>"
            "<br>"
            "<b>Lighting</b><br>"
            "18 sky presets (Preetham atmospheric model) incl. Mars, Titan, Pandora.<br>"
            "47 studio light presets. HDRI environment maps.<br>"
            "CIE standard illuminants (D65, D50, A, F2, F11).<br>"
            "<br>"
            "<b>Pipeline</b><br>"
            "sRGB / ACEScg / ACES 2065-1 output. 14 AOV passes.<br>"
            "OpenSubdiv 3.6 Catmull-Clark displacement. Motion blur.<br>"
            "Adaptive sampling. OptiX AI denoiser. Thin-lens DOF."
            "</font>"
        );
    }
    EndGroup(f);

    Divider(f);
    Text_knob(f,
        "<font color='#555' size='-1'>"
        "SpectralRenderer v1.1 \xc2\xb7 Nuke 17 \xc2\xb7 Embree 4 \xc2\xb7 OptiX 8.1 \xc2\xb7 CUDA 12.6<br>"
        "Created by Marten Blumen \xc2\xb7 github.com/bratgot/SpectralRenderer"
        "</font>"
    );

    Tab_knob(f, "Volumes");
    {
        // ─── File ───────────────────────────────────────────────────────
        File_knob(f, &_vdbFile, "vdb_file", "VDB file");
        SetFlags(f, Knob::INVISIBLE);
        Tooltip(f, "OpenVDB volume file (.vdb).\n"
                   "Supports #### frame padding for sequences.\n"
                   "Or use SpectralVDBRead -> GeoScene -> scn input.");
        Bool_knob(f, &_vdbAutoSequence, "vdb_auto_sequence", "auto sequence"); SetFlags(f, Knob::INVISIBLE);
        Int_knob(f, &_vdbFrameOffset, "vdb_frame_offset", "frame offset"); SetFlags(f, Knob::INVISIBLE);
        Obsolete_knob(f, "vdb_orig_file", nullptr);
        String_knob(f, &_vdbOrigFile, "vdb_orig_file", "");
        SetFlags(f, Knob::INVISIBLE);

        // --- Axis Controls (disabled — use GeoTransform upstream instead) ---
        // TODO: remove once GeoTransform pipeline is fully validated
        /*
        Divider(f, "Axis Controls");
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Transform the volume in world space. Rotation is around the volume centre."
            "</font>"
        );
        Newline(f);
        Double_knob(f, &_vdbTranslate[0], "vdb_translate_x", "translate"); SetRange(f, -1000, 1000);
        Double_knob(f, &_vdbTranslate[1], "vdb_translate_y", ""); SetRange(f, -1000, 1000);
        ClearFlags(f, Knob::STARTLINE);
        Double_knob(f, &_vdbTranslate[2], "vdb_translate_z", ""); SetRange(f, -1000, 1000);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "World-space offset from the VDB's native position.\nX Y Z in world units.");

        Double_knob(f, &_vdbRotate[0], "vdb_rotate_x", "rotate"); SetRange(f, -360, 360);
        Double_knob(f, &_vdbRotate[1], "vdb_rotate_y", ""); SetRange(f, -360, 360);
        ClearFlags(f, Knob::STARTLINE);
        Double_knob(f, &_vdbRotate[2], "vdb_rotate_z", ""); SetRange(f, -360, 360);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Rotation in degrees around volume centre.\nXYZ Euler order.");

        Double_knob(f, &_vdbScale[0], "vdb_scale_x", "scale"); SetRange(f, -10, 10);
        Double_knob(f, &_vdbScale[1], "vdb_scale_y", ""); SetRange(f, -10, 10);
        ClearFlags(f, Knob::STARTLINE);
        Double_knob(f, &_vdbScale[2], "vdb_scale_z", ""); SetRange(f, -10, 10);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Scale relative to volume centre.\n1 = original. Negative = mirror.");
        Double_knob(f, &_vdbUniformScale, "vdb_uniform_scale", "uniform scale");
        SetRange(f, 0.01, 10);
        Tooltip(f, "Multiplier applied to all three scale axes.\n"
                   "0.5 = half size. 2 = double size.");

        Text_knob(f, "<font size='-1' color='#666'>Rotate</font>");
        Button(f, "vdb_rot_x90", " X +90\xc2\xb0 ");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Rotate 90 degrees around X axis (pitch).");
        Button(f, "vdb_rot_y90", " Y +90\xc2\xb0 ");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Rotate 90 degrees around Y axis (yaw).");
        Button(f, "vdb_rot_z90", " Z +90\xc2\xb0 ");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Rotate 90 degrees around Z axis (roll).");
        Text_knob(f, "<font size='-1' color='#666'>Flip</font>");
        Button(f, "vdb_mirror_x", " X ");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Mirror volume along X axis (negate X scale).");
        Button(f, "vdb_mirror_y", " Y ");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Mirror volume along Y axis (negate Y scale).");
        Button(f, "vdb_mirror_z", " Z ");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Mirror volume along Z axis (negate Z scale).");
        Button(f, "vdb_reset_xform", " Reset ");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Reset translate, rotate, and scale to default.");
        */

        // --- Render Mode -------------------------------------------------
        Divider(f, "Render Mode");
        static const char* const renderModes[] = {
            "Lit", "Greyscale", "Heat", "Cool", "Blackbody", "Explosion", nullptr
        };
        Enumeration_knob(f, &_vdbRenderMode, renderModes, "vdb_render_mode", "mode");
        Tooltip(f, "Lit \xe2\x80\x94 warm density tint, uses temperature when available\n"
                   "Greyscale \xe2\x80\x94 density preview, no lighting (fastest)\n"
                   "Heat \xe2\x80\x94 warm ramp, uses temperature grid when available\n"
                   "Cool \xe2\x80\x94 cool ramp: black \xe2\x86\x92 blue \xe2\x86\x92 cyan \xe2\x86\x92 white\n"
                   "Blackbody \xe2\x80\x94 temperature \xe2\x86\x92 physical fire colour + flame emission\n"
                   "Explosion \xe2\x80\x94 fire core (temp/flame) + smoke shell (density)");
        Double_knob(f, &_vdbIntensity, "vdb_intensity", "intensity");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 0, 10);
        Tooltip(f, "Master brightness. Does not affect alpha.");
        Bool_knob(f, &_vdbSpectralVolumes, "vdb_spectral_volumes", "spectral volumes");
        Text_knob(f, "<font color='#557' size='-2'>cpu</font>");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "ON: each ray samples one wavelength (physically correct but slow,\n"
                   "needs 20+ spp for proper colour convergence).\n"
                   "OFF: direct RGB rendering (fast, full colour at 1 spp).\n"
                   "CPU only -- GPU always uses RGB mode.\n"
                   "Surfaces always use full spectral regardless of this setting.");

        // ─── Viewport controls moved to 3D Viewport tab ───────────────

        // ─── Grids (moved to SpectralVDBRead) ─────────────────────────
        // Hidden but kept for backward compatibility with saved scripts
        Button(f, "discover_grids", "Discover Grids"); SetFlags(f, Knob::INVISIBLE);
        Enumeration_knob(f, &_vdbDensityGridIdx, kVdbGridMenu, "vdb_density_grid", "density"); SetFlags(f, Knob::INVISIBLE);
        String_knob(f, &_vdbDensityOverride, "vdb_density_override", "override"); SetFlags(f, Knob::INVISIBLE);
        Enumeration_knob(f, &_vdbTempGridIdx, kVdbGridMenu, "vdb_temp_grid", "temperature"); SetFlags(f, Knob::INVISIBLE);
        String_knob(f, &_vdbTempOverride, "vdb_temp_override", "override"); SetFlags(f, Knob::INVISIBLE);
        Enumeration_knob(f, &_vdbFlameGridIdx, kVdbGridMenu, "vdb_flame_grid", "flame"); SetFlags(f, Knob::INVISIBLE);
        String_knob(f, &_vdbFlameOverride, "vdb_flame_override", "override"); SetFlags(f, Knob::INVISIBLE);
        Enumeration_knob(f, &_vdbColorGridIdx, kVdbGridMenu, "vdb_color_grid", "color"); SetFlags(f, Knob::INVISIBLE);
        String_knob(f, &_vdbColorOverride, "vdb_color_override", "override"); SetFlags(f, Knob::INVISIBLE);

        // Shading controls hidden — now on SpectralVolumeMaterial
        static const char* const volPresets[] = {"Custom",nullptr};
        Enumeration_knob(f, &_vdbShadingPreset, volPresets, "vdb_shading_preset", "Shading Preset"); SetFlags(f, Knob::INVISIBLE);
        Double_knob(f, &_vdbExtinction, "vdb_extinction", "extinction"); SetFlags(f, Knob::INVISIBLE);
        Double_knob(f, &_vdbScattering, "vdb_scattering", "scattering"); SetFlags(f, Knob::INVISIBLE);
        Double_knob(f, &_vdbDensityMult, "vdb_density_mult", "density mult"); SetFlags(f, Knob::INVISIBLE);
        Double_knob(f, &_vdbStepSize, "vdb_step_size", "step size"); SetFlags(f, Knob::INVISIBLE);
        // ─── Voxel Resolution ────────────────────────────────────────
        Divider(f, "Voxel Resolution");
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Master cap on voxel resolution across all VDBRead nodes.<br>"
            "Each VDBRead can set its own resolution, but will never<br>"
            "exceed this master setting. Lower = faster, higher = sharper."
            "</font>"
        );
        Newline(f);
        static const char* const volResOpts[] = {
            "1/8 (fastest)", "1/4 (preview)", "1/2 (production)", "Full (high)", "Native", nullptr
        };
        Enumeration_knob(f, &_vdbVolRes, volResOpts, "vdb_vol_res", "resolution");
        Tooltip(f, "Fraction of native VDB resolution to render at.\n"
                   "1/8 = ~0.2%% of native voxels (very fast, rough look)\n"
                   "1/4 = ~1.6%% of native voxels (good for layout)\n"
                   "1/2 = ~12.5%% of native voxels (production)\n"
                   "Full = ~100%% at 512\xc2\xb3 cap (high quality)\n"
                   "Native = actual VDB dims, capped at 1024\xc2\xb3");
        static const char* const scrubOpts[] = {
            "Fast (64)", "Medium (128)", "Full (no cap)", nullptr
        };
        Enumeration_knob(f, &_scrubQuality, scrubOpts, "scrub_quality", "scrub quality");
        Tooltip(f, "Volume resolution during timeline scrubbing (preview pass).\n"
                   "Fast = 64\xc2\xb3 density-only (~50ms, shape only)\n"
                   "Medium = 128\xc2\xb3 with NanoVDB (~400ms, decent detail)\n"
                   "Full = no cap, uses render resolution (slower but full quality)");
        Double_knob(f, &_vdbAnisotropy, "vdb_anisotropy", "anisotropy"); SetRange(f, -1, 1);
        SetFlags(f, Knob::INVISIBLE);

        // (Emission, Phase Function, Multiple Scatter, Procedural Noise
        //  have moved to SpectralVolumeMaterial node)

        // ─── Quality ────────────────────────────────────────────────────
        Divider(f, "Quality");
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Start with a preset, then fine-tune. Draft for layout, Final for delivery."
            "</font>"
        );
        Newline(f);
        static const char* const qualPresets[] = {
            "Custom", "Draft", "Preview", "Production", "Final", "Ultra", nullptr
        };
        Enumeration_knob(f, &_vdbQualityPreset, qualPresets, "vdb_quality_preset", "preset");
        Tooltip(f, "Draft = fastest preview\n"
                   "Preview = quick turnaround\n"
                   "Production = client review\n"
                   "Final = delivery quality\n"
                   "Ultra = maximum fidelity");
        Double_knob(f, &_vdbQuality, "vdb_quality", "quality"); SetRange(f, 1, 10);
        Tooltip(f, "Step resolution. Step = voxelSize / (quality\xc2\xb2 \xc3\x97 0.25).\n"
                   "1 = fast. 5 = good. 7 = high. 10 = final.");
        Bool_knob(f, &_vdbAdaptiveStep, "vdb_adaptive_step", "adaptive");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "4x larger steps in empty regions. 2-4x speedup.");
        Int_knob(f, &_vdbShadowSteps, "vdb_shadow_steps", "shadow steps"); SetRange(f, 0, 64);
        Tooltip(f, "Shadow ray samples per light.\n"
                   "0 = no self-shadowing. 4-8 = preview. 16-32 = final.");
        Double_knob(f, &_vdbShadowDensity, "vdb_shadow_density", "shadow density");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 5);
        Tooltip(f, "Extinction multiplier for shadow rays.\n"
                   "1 = physical. <1 = lighter. >1 = darker. 0 = none.");

        // ─── Shadow Cache ───────────────────────────────────────────────
        BeginClosedGroup(f, "vdb_shcache", "Shadow Performance");
        {
            Text_knob(f,
                "<font color='#777' size='-1'>"
                "Precomputes transmittance per directional light.<br>"
                "Reduces shadow cost from O(steps) to O(1) per sample."
                "</font>"
            );
            Newline(f);
            Bool_knob(f, &_vdbShadowCache, "vdb_shadow_cache", "shadow cache");
            Tooltip(f, "5-10x faster shadow evaluation.\n"
                       "Rebuilt when lights or VDB change.");
            static const char* const cacheRes[] = {
                "Full (1:1)", "Half (2:1)", "Quarter (4:1)", nullptr
            };
            Enumeration_knob(f, &_vdbShadowCacheRes, cacheRes, "vdb_shadow_cache_res", "");
            ClearFlags(f, Knob::STARTLINE);
        }
        EndGroup(f);

    }

    // =====================================================================
    // 3D VIEWPORT TAB
    // =====================================================================
    Tab_knob(f, "3D Viewport");

    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Real-time preview of volumes and geometry in Nuke\xe2\x80\x99s 3D viewer.<br>"
        "All settings here are viewport-only \xe2\x80\x94 they do not affect the final render."
        "</font>"
    );

    // ─── Master Switch ──────────────────────────────────────────────
    Divider(f, "Display");
    Bool_knob(f, &_vdb3dPreview, "vdb_3d_preview", "enable 3D preview");
    Tooltip(f, "Master switch for all 3D viewport drawing.\n\n"
               "When OFF, no volume or geometry preview appears in the viewer.\n"
               "Turn off to speed up the viewport when not needed.");

    // ─── Volume Preview ─────────────────────────────────────────────
    Divider(f, "Volume Preview");
    Bool_knob(f, &_vdbShowBbox, "vdb_show_bbox", "bounding box");
    Tooltip(f, "Green wireframe bounding box around the volume.\n\n"
               "Useful for camera framing and positioning\n"
               "before enabling the full volume preview.");
    Bool_knob(f, &_vdbShowPoints, "vdb_show_points", "point cloud");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Coloured point cloud sampled from the volume density grid.\n\n"
               "Colour follows the current Volume Render Mode.\n"
               "Fast and lightweight \xe2\x80\x94 good for scrubbing timelines.");
    Bool_knob(f, &_vdbShadedPreview, "vdb_shaded", "shaded volume");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "GPU ray-marched volume in the 3D viewport.\n\n"
               "Renders absorption, self-shadowing, and fire emission\n"
               "using the connected SpectralEnvLight or built-in sun.\n\n"
               "Tip: Disable \xe2\x80\x98point cloud\xe2\x80\x99 when using shaded for a cleaner look.");
    Newline(f);
    {
        static const char* const vpResOpts[] = {
            "32", "64", "128", "256", nullptr
        };
        Enumeration_knob(f, &_vdbViewportRes, vpResOpts, "vdb_viewport_res", "volume res");
        Tooltip(f, "Resolution of the 3D volume texture used for viewport preview.\n\n"
                   "32 = fastest scrubbing, blocky appearance\n"
                   "64 = good balance for animation work\n"
                   "128 = detailed preview (default)\n"
                   "256 = near-render quality, slower to load\n\n"
                   "Higher values also load temperature and flame grids\n"
                   "for fire and explosion colour previews.");
    }
    Double_knob(f, &_vdbPointDensity, "vdb_point_density", "point density");
    SetRange(f, 0.1, 1.0);
    Tooltip(f, "Density of the point cloud sampling.\n\n"
               "0.1 = very sparse (fast)\n"
               "1.0 = all voxels (slow)\n\n"
               "Only affects point cloud mode, not shaded preview.");
    Double_knob(f, &_vdbPointSize, "vdb_point_size", "point size");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 1, 8);
    Tooltip(f, "GL point size in pixels for the point cloud display.");

    // ─── Viewport Lighting ──────────────────────────────────────────
    Divider(f, "Viewport Lighting");
    {
        static const char* const pcfOpts[] = {
            "sharp", "3x3", "5x5", "7x7", nullptr
        };
        Enumeration_knob(f, &_vpShadowPCF, pcfOpts, "vp_shadow_pcf", "shadow quality");
        Tooltip(f, "Viewport shadow map filtering.\n\n"
                   "sharp = hard single-sample shadow\n"
                   "3x3 = light penumbra (9 samples)\n"
                   "5x5 = soft shadow (25 samples, default)\n"
                   "7x7 = very soft (49 samples)\n\n"
                   "Combine with the SpectralEnvLight shadow softness knob\n"
                   "to control penumbra width in the viewport.");
    }
    {
        static const char* const volShadOpts[] = {
            "off", "4", "8", "16", nullptr
        };
        Enumeration_knob(f, &_vpVolShadowSamples, volShadOpts, "vp_vol_shadow", "volume shadow");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Volume shadow samples cast onto geometry surfaces.\n\n"
                   "Marches through the volume toward the light to\n"
                   "determine how much light is blocked by the cloud.\n\n"
                   "off = no volume shadows on geometry\n"
                   "4 = fast preview\n"
                   "8 = default\n"
                   "16 = best quality");
    }

    // ─── Viewport Reflections ───────────────────────────────────────
    Divider(f, "Viewport Reflections");
    Bool_knob(f, &_vpEnvReflections, "vp_env_reflections", "environment");
    Tooltip(f, "Sky environment reflections on metallic and specular surfaces.\n\n"
               "Includes procedural sky dome gradient, sun corona,\n"
               "Fresnel falloff, and GGX specular highlights.");
    Bool_knob(f, &_vpGeoReflections, "vp_geo_reflections", "geometry");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "One-bounce screen-space reflections of other geometry.\n\n"
               "Renders a reflected-camera pre-pass then samples it.\n"
               "Best on smooth metallic surfaces facing other objects.");
    Bool_knob(f, &_vpVolReflections, "vp_vol_reflections", "volumes");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Include volumes in viewport geometry reflections.\n\n"
               "Volumes are drawn into the reflection pre-pass FBO\n"
               "so they appear in nearby reflective surfaces.");
    {
        static const char* const reflOpts[] = {
            "8 steps", "16 steps", "32 steps", "64 steps", nullptr
        };
        Enumeration_knob(f, &_vpReflSteps, reflOpts, "vp_refl_steps", "quality");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Screen-space ray march steps for geometry reflections.\n\n"
                   "More steps = longer reflection reach but slower.\n"
                   "8 is fast, 32 is a good balance, 64 for hero shots.");
    }

    // ─── Performance ────────────────────────────────────────────────
    Divider(f, "Performance");
    Bool_knob(f, &_vdbFastScrub, "vdb_fast_scrub", "fast scrub");
    Tooltip(f, "Show bounding box only during timeline scrub.\n\n"
               "Full preview loads automatically when playback stops.\n"
               "Keeps the timeline responsive with heavy VDB sequences.");
    Bool_knob(f, &_vdbCacheEnabled, "vdb_cache_enabled", "frame cache");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Cache loaded VDB frames in memory for instant scrub-back.\n\n"
               "Each cached frame stores the resampled density grid.\n"
               "Useful for reviewing animation without re-loading from disk.");
    Int_knob(f, &_vdbCacheMax, "vdb_cache_max", "");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 1, 32);
    Text_knob(f, "frames");
    ClearFlags(f, Knob::STARTLINE);
    Button(f, "vdb_cache_clear", "Clear Cache");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Flush all cached VDB frames from memory.");

    // ─── Artist Notes ───────────────────────────────────────────────
    Divider(f, "");
    Text_knob(f,
        "<font color='#556' size='-1'>"
        "Viewport tips:<br>"
        "<br>"
        "\xe2\x80\xa2 Shaded preview uses the SpectralEnvLight sun direction and colour.<br>"
        "\xe2\x80\xa2 Without an EnvLight, the built-in light on the Lighting tab controls the sun.<br>"
        "<br>"
        "\xe2\x80\xa2 Reflections show one bounce of geometry in screen space.<br>"
        "\xe2\x80\xa2 Only surfaces visible in the viewport can reflect \xe2\x80\x94 off-screen objects won\xe2\x80\x99t appear.<br>"
        "<br>"
        "\xe2\x80\xa2 Viewport shadows and reflections are approximate previews.<br>"
        "\xe2\x80\xa2 The final render uses full spectral path tracing with unlimited bounces.<br>"
        "<br>"
        "\xe2\x80\xa2 High volume res (256) with shaded preview can be slow \xe2\x80\x94 use 64 for animation work.<br>"
        "\xe2\x80\xa2 Guide overlays (dome arc, sun path, compass) are controlled on the SpectralEnvLight node."
        "</font>"
    );

    // ───────────────────────────────────────────────────────────────
    // Motion Blur tab
    // All motion blur controls live here: shutter, object (geometry)
    // motion blur, volume velocity blur, camera blur (when wired up).
    // ───────────────────────────────────────────────────────────────
    Tab_knob(f, "Motion Blur");

    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Motion blur is driven by the shutter interval.<br>"
        "Shutter=0 disables all motion blur.<br>"
        "Object and volume contributions are independent toggles below."
        "</font>"
    );

    Divider(f, "Shutter");
    {
        // Presets are 180-degree shutter equivalents (half-frame exposure)
        // applied via knob_changed when the preset index changes. "Custom"
        // disables auto-apply so the user can type arbitrary values.
        static const char* const shutP[] = {
            "Start (0 to 0.5)",
            "Centre (-0.25 to 0.25)",
            "End (-0.5 to 0)",
            "Custom",
            nullptr
        };
        Enumeration_knob(f, &_shutterPreset, shutP, "shutter_preset", "preset");
        Tooltip(f, "Shutter offset preset (all are 180 shutter = half-frame exposure):\n"
                   "  Start  - exposure begins at frame  (blur leads ahead of motion)\n"
                   "  Centre - exposure centred on frame (blur even on both sides, default)\n"
                   "  End    - exposure ends at frame    (blur trails behind motion)\n"
                   "  Custom - do not override open/close below");
        Newline(f);
        Double_knob(f, &_shutterOpen, "shutter_open", "open"); SetRange(f, -1, 0);
        Tooltip(f, "Shutter open time relative to frame, in frames.\n"
                   "-0.25 = one quarter frame before current (default, 180 shutter centred).\n"
                   "-0.5  = one half   frame before current (360 shutter centred, full blur).\n"
                   " 0.0  = exposure starts at current frame.\n"
                   "Equal to shutter close disables motion blur entirely.");
        Double_knob(f, &_shutterClose, "shutter_close", "close"); SetRange(f, 0, 1);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Shutter close time relative to frame, in frames.\n"
                   "0.25 = one quarter frame after current (default, 180 shutter centred).\n"
                   "0.5  = one half   frame after current (360 shutter centred, full blur).");
        Int_knob(f, &_motionSamples, "motion_samples", "samples"); SetRange(f, 2, 8);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Volume-velocity time samples across the shutter.\n"
                   "2 = fast. 3 = good. 5 = smooth. 8 = overkill.\n"
                   "Object motion blur doesn't use this -- it samples time\n"
                   "once per spp, so raise the main spp for smoother blur.");
    }

    Divider(f, "Object motion blur");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Geometry deformation and rigid body transforms on animated meshes."
            "</font>"
        );
        Newline(f);
        Bool_knob(f, &_objectMotionBlur, "object_motion_blur", "enable");
        Tooltip(f, "Motion blur from vertex deformation and rigid body\n"
                   "transforms on animated meshes.\n"
                   "\n"
                   "Disable for reference frames where motion should be\n"
                   "frozen without changing shutter settings.\n"
                   "\n"
                   "CPU: fully supported.\n"
                   "GPU: currently falls back to CPU when active\n"
                   "(OptiX motion GAS support landing in next patch).");
    }

    Divider(f, "Volume velocity blur");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "VDB velocity grids (vel/v) drive volume sample motion across the shutter."
            "</font>"
        );
        Newline(f);
        Bool_knob(f, &_motionBlur, "motion_blur", "enable");
        Tooltip(f, "Render volume motion blur from VDB velocity grids.\n"
                   "Offsets volume samples across the shutter interval.\n"
                   "Requires a velocity grid to be loaded.\n"
                   "\n"
                   "This is NOT the same as object motion blur above --\n"
                   "this is purely for volumes that carry per-voxel velocity data.");
    }

    // Camera motion blur kept INVISIBLE until the axis-chain evaluation is wired.
    // Knob definitions live here so their values load/save from existing .nk files.
    Bool_knob(f, &_cameraMblur, "camera_mblur", "camera blur");
    SetFlags(f, Knob::INVISIBLE);
    Int_knob(f, &_cameraMblurQuality, "camera_mblur_quality", "quality");
    SetFlags(f, Knob::INVISIBLE);

    Tab_knob(f, "AOV");

    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Arbitrary Output Variables for compositing, relighting, and denoising.<br>"
        "Each enabled pass appears as a separate layer in the EXR output.<br>"
        "Access via Shuffle or Copy nodes downstream."
        "</font>"
    );

    // ─── Geometry AOVs ──────────────────────────────────────────────
    Divider(f, "Geometry");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Surface data passes from geometry renders. Used for relighting,<br>"
        "contact shadows, and CG integration in comp."
        "</font>"
    );
    Newline(f);
    Bool_knob(f, &_aovNormals, "aov_normals", "N (normals)");
    Tooltip(f, "World-space surface normals as aov_N (RGB).\n"
               "R=X, G=Y, B=Z. Range -1 to 1, remapped to 0-1.\n"
               "Use for relighting, edge detection, and rim mattes in comp.");
    Bool_knob(f, &_aovPosition, "aov_position", "P (position)");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "World-space hit position as aov_P (RGB = XYZ).\n"
               "32-bit float. Use for world-space effects,\n"
               "position-based grading, and depth reconstruction.");
    Bool_knob(f, &_aovPRef, "aov_pref", "Pref");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Reference position (pre-deformation) as aov_Pref.\n"
               "Object-space coordinates before animation.\n"
               "Use for projection mapping that sticks to deforming surfaces.");
    Newline(f);
    Bool_knob(f, &_aovUV, "aov_uv", "UV");
    Tooltip(f, "Texture coordinates as aov_UV (RG = U,V).\n"
               "Use for texture-space effects or UV-based mattes.");
    Bool_knob(f, &_aovAlbedo, "aov_albedo", "albedo");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Surface albedo (unlit base colour) as aov_albedo.\n"
               "The material colour before any lighting.\n"
               "Useful for colour grading without affecting lighting.");
    Bool_knob(f, &_aovDepth, "aov_depth", "depth (Z)");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Camera-space Z as depth.Z (single channel).\n"
               "On by default -- Nuke's ZDefocus, ZBlur and ZMerge all\n"
               "read this channel. Values are positive metres (or scene\n"
               "units) going away from the camera. Background pixels\n"
               "read 1e30 (far plane sentinel).");

    Divider(f, "Lighting decomposition");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "beauty = direct + indirect + emission. Adjust each in comp<br>"
        "to relight without re-rendering."
        "</font>"
    );
    Newline(f);
    Bool_knob(f, &_aovDirect, "aov_direct", "direct");
    Tooltip(f, "All direct lighting (light -> surface -> camera).\n"
               "Includes all light types. Multiply to dim/boost key lights.");
    Bool_knob(f, &_aovIndirect, "aov_indirect", "indirect");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "All indirect lighting (light -> bounce -> surface -> camera).\n"
               "GI, reflections, colour bleed. Multiply to adjust bounce light.");
    Bool_knob(f, &_aovEmission, "aov_emission", "emission");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Self-illumination (glowing materials, fire, neon).\n"
               "Add to adjust glow intensity in comp.");

    BeginClosedGroup(f, "aov_lpe", "LPE decomposition (advanced)");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Light Path Expressions split direct/indirect by BSDF lobe.<br>"
            "Gives fine control over specular highlights vs diffuse fill."
            "</font>"
        );
        Newline(f);
        Bool_knob(f, &_aovDiffuseDirect, "aov_diffuse_direct", "diffuse direct");
        Tooltip(f, "LPE: C<RD>L -- direct diffuse only.\n"
                   "Matte lighting without specular highlights.");
        Bool_knob(f, &_aovSpecularDirect, "aov_specular_direct", "specular direct");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "LPE: C<RS>L -- direct specular highlights only.\n"
                   "Useful for adjusting highlight intensity/colour.");
        Newline(f);
        Bool_knob(f, &_aovDiffuseIndirect, "aov_diffuse_indirect", "diffuse indirect");
        Tooltip(f, "LPE: C<RD>+L -- bounced diffuse (colour bleed, GI).");
        Bool_knob(f, &_aovSpecularIndirect, "aov_specular_indirect", "specular indirect");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "LPE: C<RS>+L -- bounced specular (reflections).");
        Newline(f);
        Bool_knob(f, &_aovTransmission, "aov_transmission", "transmission");
        Tooltip(f, "LPE: C<TS>L -- light transmitted through glass/SSS.\n"
                   "Refraction, subsurface scatter, translucency.");
    }
    EndGroup(f);

    BeginClosedGroup(f, "aov_util", "Utility passes");
    {
        Int_knob(f, &_aoSamples, "ao_samples", "AO samples"); SetRange(f, 0, 64);
        Tooltip(f, "Ambient occlusion ray count. 0 = disabled.\n"
                   "8 = fast preview. 32 = smooth. 64 = final.\n"
                   "Output as aov_AO (greyscale contact shadow).");
        Float_knob(f, &_aoRadius, "ao_radius", "AO radius"); SetRange(f, 0.1f, 100.f);
        Tooltip(f, "Maximum distance for AO rays (world units).\n"
                   "Small = tight contact shadows. Large = broad ambient darkening.");
    }
    EndGroup(f);

    // ─── Volume AOVs ────────────────────────────────────────────────
    Divider(f, "Volume");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Volume-specific passes. Each appears as a separate layer.<br>"
        "Use Shuffle to isolate, Grade to adjust, Merge to composite."
        "</font>"
    );
    Newline(f);
    Bool_knob(f, &_aovVolDensity, "aov_vol_density", "density");
    Tooltip(f, "Integrated volume density as vol_density (greyscale).\n"
               "Equivalent to beauty alpha. Use for holdout mattes,\n"
               "density grading, or as a luma matte for the volume.");
    Bool_knob(f, &_aovVolEmission, "aov_vol_emission", "emission");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Volume emission as vol_emission (RGB).\n"
               "Fire and blackbody glow before background compositing.\n"
               "Grade this to adjust fire brightness without re-rendering.");
    Bool_knob(f, &_aovVolShadow, "aov_vol_shadow", "shadow");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Self-shadow transmittance as vol_shadow (greyscale).\n"
               "1.0 = fully lit, 0.0 = fully self-shadowed.\n"
               "Multiply with beauty to deepen/lighten shadows in comp.");
    Newline(f);
    Bool_knob(f, &_aovVolLights, "aov_vol_lights", "per-light (x4)");
    Tooltip(f, "Individual light contributions as vol_light0-3 (RGB).\n"
               "Each layer = scatter from one light source.\n"
               "Grade each to relight volumes in comp without re-rendering.");
    Bool_knob(f, &_aovVolDepth, "aov_vol_depth", "depth");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "First-hit depth as vol_depth (single channel).\n"
               "Distance from camera to first dense voxel.\n"
               "Use for depth-of-field, fog cards, or Z-compositing.");
    Bool_knob(f, &_aovMotion, "aov_motion", "motion vectors");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Screen-space motion vectors as vol_motion (RG).\n"
               "Pixels/frame displacement. Requires velocity grid.\n"
               "Feed to VectorBlur or MotionBlur nodes in comp.");

    // ─── Denoiser ───────────────────────────────────────────────────
    Divider(f, "Denoiser auxiliary");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Feed these to a downstream Denoise node alongside beauty.<br>"
        "Works for both geometry and volume renders."
        "</font>"
    );
    Newline(f);
    Bool_knob(f, &_aovDenoiseAlbedo, "aov_denoise_albedo", "denoise albedo");
    Tooltip(f, "Unlit surface/volume albedo as vol_denoise_albedo.\n"
               "Helps the denoiser separate noise from texture detail.");
    Bool_knob(f, &_aovDenoiseNormal, "aov_denoise_normal", "denoise normal");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Surface/density-gradient normals as vol_denoise_normal.\n"
               "Helps the denoiser preserve edges and surface detail.");

    // ─── Deep Output ────────────────────────────────────────────────
    Divider(f, "Deep output");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Generates depth-sorted RGBA slabs for deep compositing.<br>"
        "Connect to DeepMerge for volume-over-geometry setups."
        "</font>"
    );
    Newline(f);
    Int_knob(f, &_deepSamples, "deep_samples", "deep samples"); SetRange(f, 0, 128);
    Tooltip(f, "Deep quality. 0 = disabled (shell only).\n"
               "16 = fast preview. 32 = 1:1 voxel stepping. 64+ = sub-voxel.\n"
               "Samples placed at density, not uniform slabs.\n"
               "More samples cluster where volume is dense,\n"
               "empty space is skipped entirely.");

}

int SpectralRenderIop::knob_changed(Knob* k)
{
    // Only reset render for knobs that affect the image
    // Skip UI-only knobs (showPanel, hidePanel, label, tile_color, etc.)
    bool isUIOnly = k->is("showPanel") || k->is("hidePanel")
                 || k->is("label") || k->is("tile_color")
                 || k->is("note_font") || k->is("note_font_size")
                 || k->is("note_font_color") || k->is("selected")
                 || k->is("help") || k->is("indicators")
                 || k->is("postage_stamp");

    if (!isUIOnly) {
        _asyncCancel.store(true);
        if (_asyncQualityThread.joinable()) _asyncQualityThread.join();
        _frameReady.store(false); _sceneReady.store(false);
        _progressiveSppDone = 0;
    }

    // Update DAG node tile colour based on device mode.
    // (The CPU/GPU/AUTO text label was dropped at user request — the
    // device is still visible via tile colour alone.)
    if (k->is("device_mode") || k->is("showPanel")) {
        int idx = std::max(0, std::min(_deviceMode, 2));

        // Tile color: CPU=blue tint, GPU=green tint, Auto=amber tint
        unsigned int tileColors[] = { 0x1a3a5aFF, 0x1a4a2aFF, 0x3a3a1aFF };
        std::string nm(node_name());
        std::string cmd = "import nuke; n = nuke.toNode('" + nm + "')\n"
                          "if n: n['tile_color'].setValue(" + std::to_string(tileColors[idx]) + ")";
        script_command(cmd.c_str(), true, false);

        // Clear any label that a previous version of the node may have set.
        if (Knob* lk = knob("label")) lk->set_text("");
        if (k->is("device_mode")) return 1;
    }

    // Shutter preset -> apply 180-degree shutter values (0.5-frame exposure)
    // to open/close knobs. "Custom" leaves them alone. Writing the knob
    // values here lets the user see the numbers update in the panel and
    // still tweak them manually afterwards (doing so will bounce the enum
    // to "Custom" only if they pick values that don't match a preset --
    // we don't force that, the enum just stays on whatever they last picked).
    if (k->is("shutter_preset")) {
        Knob* kOpen  = knob("shutter_open");
        Knob* kClose = knob("shutter_close");
        if (kOpen && kClose) {
            switch (_shutterPreset) {
                case 0:  // Start: exposure begins at frame
                    kOpen->set_value(0.0);
                    kClose->set_value(0.5);
                    break;
                case 1:  // Centre: exposure centred on frame (default)
                    kOpen->set_value(-0.25);
                    kClose->set_value(0.25);
                    break;
                case 2:  // End: exposure ends at frame
                    kOpen->set_value(-0.5);
                    kClose->set_value(0.0);
                    break;
                case 3:  // Custom: leave knob values alone
                default:
                    break;
            }
        }
        return 1;
    }

    // VDB auto-sequence
    if (k->is("vdb_auto_sequence")) {
        if (_vdbAutoSequence) {
            std::string p(_vdbFile ? _vdbFile : "");
            if (Knob* ok = knob("vdb_orig_file")) ok->set_text(p.c_str());
            if (!p.empty()) {
                size_t dot = p.rfind(".vdb");
                if (dot == std::string::npos) dot = p.rfind(".VDB");
                if (dot != std::string::npos) {
                    size_t end = dot, start = end;
                    while (start > 0 && p[start-1] >= '0' && p[start-1] <= '9') --start;
                    if (start < end) {
                        int ndig = int(end - start); if (ndig < 4) ndig = 4;
                        p = p.substr(0, start) + std::string(ndig, '#') + p.substr(end);
                        if (Knob* fk = knob("vdb_file")) fk->set_text(p.c_str());
                    }
                }
            }
        } else {
            const char* orig = _vdbOrigFile ? _vdbOrigFile : "";
            if (orig[0]) {
                if (Knob* fk = knob("vdb_file")) fk->set_text(orig);
                if (Knob* ok = knob("vdb_orig_file")) ok->set_text("");
            }
        }
        return 1;
    }
    if (k->is("vdb_frame_offset") || k->is("vdb_file")) {
        _frameReady.store(false); _sceneReady.store(false);
        _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
        _vdbLoadedPath.clear();
        _volume.reset();  // force reload
        // Re-detect sequence if auto is on
        if (_vdbAutoSequence && k->is("vdb_file")) {
            if (Knob* ok = knob("vdb_orig_file")) ok->set_text(_vdbFile ? _vdbFile : "");
        }
        return 1;
    }
    if (k->is("scanline_compat") || k->is("vdb_3d_preview") || k->is("vdb_show_points") || k->is("vdb_shaded") || k->is("vdb_viewport_res") || k->is("vdb_point_density") || k->is("vdb_point_size") || k->is("vdb_show_bbox") || k->is("vdb_fast_scrub") || k->is("vp_shadow_pcf") || k->is("vp_vol_shadow") || k->is("vp_env_reflections") || k->is("vp_geo_reflections") || k->is("vp_vol_reflections") || k->is("vp_refl_steps")) {
        _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
        if (k->is("vdb_fast_scrub") || k->is("vdb_viewport_res")) _vdbLoadedPath.clear();
        return 1;
    }
    if (k->is("vdb_cache_clear")) {
        _vdbCache.clear(); _vdbCacheLRU.clear();
        return 1;
    }
    if (k->is("vdb_cache_enabled") && !_vdbCacheEnabled) {
        _vdbCache.clear(); _vdbCacheLRU.clear();
        return 1;
    }
    if (k->is("discover_grids")) {
#ifdef SPECTRAL_HAS_VDB
        if (!_vdbFile || strlen(_vdbFile) == 0) { error("No VDB file."); return 1; }
        int frame = int(outputContext().frame()) + _vdbFrameOffset;
        std::string resolvedPath = _vdbAutoSequence ? _resolveFramePath(frame) : std::string(_vdbFile);
        auto grids = pxr::SpectralVDBLoader::DiscoverGrids(resolvedPath.c_str());
        if (grids.empty()) { error("No grids in %s", resolvedPath.c_str()); return 1; }
        std::string bestD, bestT, bestF, bestC;
        for (const auto& g : grids) {
            if (bestD.empty() && g.category == "density") bestD = g.name;
            if (bestT.empty() && g.category == "temperature") bestT = g.name;
            if (bestF.empty() && g.category == "flame") bestF = g.name;
            if (bestC.empty() && g.category == "color") bestC = g.name;
        }
        if (bestD.empty()) for (const auto& g : grids) if (g.type == "float") { bestD = g.name; break; }
        if (!bestD.empty() && knob("vdb_density_override")) knob("vdb_density_override")->set_text(bestD.c_str());
        if (!bestT.empty() && knob("vdb_temp_override")) knob("vdb_temp_override")->set_text(bestT.c_str());
        if (!bestF.empty() && knob("vdb_flame_override")) knob("vdb_flame_override")->set_text(bestF.c_str());
        if (!bestC.empty() && knob("vdb_color_override")) knob("vdb_color_override")->set_text(bestC.c_str());
        if ((!bestT.empty() || !bestF.empty()) && knob("vdb_shading_preset")) knob("vdb_shading_preset")->set_value(2);
        std::string msg;
        for (const auto& g : grids) { msg += g.name + " (" + g.type + ")\\n"; }
        if (!bestD.empty()) msg += "\\nDensity: " + bestD;
        if (!bestT.empty()) msg += "\\nTemperature: " + bestT;
        script_command(("python {nuke.message('" + msg + "')}").c_str());
        _vdbLoadedPath.clear(); _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
        _frameReady.store(false); _sceneReady.store(false);
#else
        error("OpenVDB not compiled.");
#endif
        return 1;
    }

    // Sky preset
    if (k->is("sky_preset")) {
        // Intensity values are squared in _BuildLightRig
        // sunR/G/B derived from sunColorFromElevation + turbidity
        switch (_skyPreset) {
            case 0: _skyMix = 0; break;  // Off
            // Earth
            case 2:  _sunElevation=60; _sunAzimuth=180; _sunIntensity=8;   _skyIntensity=0.5;  _turbidity=2.5; _skyMix=1; break; // Clear Day
            case 3:  _sunElevation=8;  _sunAzimuth=240; _sunIntensity=6;   _skyIntensity=0.2;  _turbidity=4;   _skyMix=1; break; // Golden Hour
            case 4:  _sunElevation=1;  _sunAzimuth=90;  _sunIntensity=5;   _skyIntensity=0.15; _turbidity=7;   _skyMix=1; break; // Red Sky Dawn
            case 5:  _sunElevation=5;  _sunAzimuth=100; _sunIntensity=5.5; _skyIntensity=0.2;  _turbidity=5;   _skyMix=1; break; // Sunrise
            case 6:  _sunElevation=50; _sunAzimuth=180; _sunIntensity=3;   _skyIntensity=1.2;  _turbidity=8;   _skyMix=1; break; // Overcast
            case 7:  _sunElevation=2;  _sunAzimuth=270; _sunIntensity=2;   _skyIntensity=0.3;  _turbidity=3;   _skyMix=1; break; // Blue Hour
            case 8:  _sunElevation=30; _sunAzimuth=180; _sunIntensity=0.8; _skyIntensity=0.08; _turbidity=2;   _skyMix=0.4; break; // Moonlit
            case 9:  _sunElevation=30; _sunAzimuth=180; _sunIntensity=0.1; _skyIntensity=0.01; _turbidity=2;   _skyMix=0.1; break; // Starlight
            case 10: _sunElevation=55; _sunAzimuth=180; _sunIntensity=9;   _skyIntensity=0.6;  _turbidity=1.8; _skyMix=1; break; // Alpine Light
            case 11: _sunElevation=80; _sunAzimuth=180; _sunIntensity=10;  _skyIntensity=0.3;  _turbidity=6;   _skyMix=1; break; // Desert Noon
            case 12: _sunElevation=3;  _sunAzimuth=180; _sunIntensity=3;   _skyIntensity=0.2;  _turbidity=2;   _skyMix=1; break; // Arctic Twilight
            // Planets
            case 13: _sunElevation=35; _sunAzimuth=200; _sunIntensity=4;   _skyIntensity=0.2;  _turbidity=9;   _skyMix=1; break; // Mars
            case 14: _sunElevation=25; _sunAzimuth=180; _sunIntensity=1;   _skyIntensity=0.15; _turbidity=10;  _skyMix=1; break; // Titan
            case 15: _sunElevation=20; _sunAzimuth=160; _sunIntensity=3;   _skyIntensity=0.2;  _turbidity=4;   _skyMix=1; break; // Krypton
            case 16: _sunElevation=15; _sunAzimuth=240; _sunIntensity=5;   _skyIntensity=0.25; _turbidity=4;   _skyMix=1; break; // Tatooine
            case 17: _sunElevation=40; _sunAzimuth=180; _sunIntensity=3;   _skyIntensity=0.4;  _turbidity=3;   _skyMix=1; break; // Pandora
            default: break;
        }
        if (Knob* mk = knob("sky_mix")) mk->set_value(_skyMix);
        if (Knob* mk = knob("sun_elevation")) mk->set_value(_sunElevation);
        if (Knob* mk = knob("sun_azimuth")) mk->set_value(_sunAzimuth);
        if (Knob* mk = knob("sun_intensity")) mk->set_value(_sunIntensity);
        if (Knob* mk = knob("sky_intensity")) mk->set_value(_skyIntensity);
        if (Knob* mk = knob("turbidity")) mk->set_value(_turbidity);
        return 1;
    }

    // Studio preset
    if (k->is("studio_preset")) {
        switch (_studioPreset) {
            case 0: _studioMix = 0; break; // Off
            case 1: _studioKeyAzimuth=45; _studioKeyElevation=30; _studioKeyIntensity=4; _studioFillRatio=0.5; _studioRimIntensity=1.5; _studioMix=1; break; // Portrait
            case 2: _studioKeyAzimuth=60; _studioKeyElevation=40; _studioKeyIntensity=6; _studioFillRatio=0.3; _studioRimIntensity=3; _studioMix=1; break; // Product
            case 3: _studioKeyAzimuth=80; _studioKeyElevation=45; _studioKeyIntensity=8; _studioFillRatio=0.1; _studioRimIntensity=4; _studioMix=1; break; // Dramatic
            case 4: _studioKeyAzimuth=30; _studioKeyElevation=25; _studioKeyIntensity=3; _studioFillRatio=0.6; _studioRimIntensity=1; _studioMix=1; _shadowSoftness=0.5; break; // Softbox
            default: break;
        }
        if (Knob* mk = knob("studio_mix")) mk->set_value(_studioMix);
        if (Knob* mk = knob("studio_key_azimuth")) mk->set_value(_studioKeyAzimuth);
        if (Knob* mk = knob("studio_key_elevation")) mk->set_value(_studioKeyElevation);
        if (Knob* mk = knob("studio_key_intensity")) mk->set_value(_studioKeyIntensity);
        if (Knob* mk = knob("studio_fill_ratio")) mk->set_value(_studioFillRatio);
        if (Knob* mk = knob("studio_rim_intensity")) mk->set_value(_studioRimIntensity);
        if (Knob* mk = knob("shadow_softness")) mk->set_value(_shadowSoftness);
        return 1;
    }

    // Location preset — set lat/long from named city
    if (k->is("location_preset") && _locationPreset > 0) {
        static const double locs[][2] = {
            {0,0},                // 0: Custom
            {51.5074, -0.1278},   // London
            {40.7128, -74.006},   // New York
            {34.0522, -118.244},  // Los Angeles
            {35.6762, 139.6503},  // Tokyo
            {-33.8688, 151.2093}, // Sydney
            {48.8566, 2.3522},    // Paris
            {25.2048, 55.2708},   // Dubai
            {64.1466, -21.9426},  // Reykjavik
            {-33.9249, 18.4241},  // Cape Town
            {19.076, 72.8777},    // Mumbai
            {90.0, 0.0},          // North Pole
            {-0.1807, -78.4678},  // Quito (Equator)
        };
        if (_locationPreset < 13) {
            _latitude = locs[_locationPreset][0];
            _longitude = locs[_locationPreset][1];
            if (knob("latitude")) knob("latitude")->set_value(_latitude);
            if (knob("longitude")) knob("longitude")->set_value(_longitude);
        }
        return 1;
    }

    // Solar position algorithm — compute elevation/azimuth from lat/long/time/day
    if (k->is("apply_sun_position")) {
        // Solar declination (Spencer 1971)
        double B = (360.0 / 365.0) * (_dayOfYear - 81) * M_PI / 180.0;
        double dec = 23.45 * std::sin(B);
        // Hour angle: 15° per hour from solar noon
        double hourAngle = (_timeOfDay - 12.0) * 15.0;
        double latR = _latitude * M_PI / 180.0;
        double decR = dec * M_PI / 180.0;
        double haR = hourAngle * M_PI / 180.0;
        // Solar elevation
        double sinElev = std::sin(latR) * std::sin(decR) + std::cos(latR) * std::cos(decR) * std::cos(haR);
        _sunElevation = std::asin(std::max(-1.0, std::min(1.0, sinElev))) * 180.0 / M_PI;
        // Solar azimuth
        double cosElev = std::cos(_sunElevation * M_PI / 180.0);
        if (cosElev > 1e-6) {
            double cosAz = (std::sin(decR) - std::sin(latR) * sinElev) / (std::cos(latR) * cosElev);
            double az = std::acos(std::max(-1.0, std::min(1.0, cosAz))) * 180.0 / M_PI;
            _sunAzimuth = (hourAngle > 0) ? 360.0 - az : az;
        }
        if (knob("sun_elevation")) knob("sun_elevation")->set_value(_sunElevation);
        if (knob("sun_azimuth")) knob("sun_azimuth")->set_value(_sunAzimuth);
        SLOG("SpectralRender: sun position — elev=%.1f° az=%.1f° (lat=%.2f lon=%.2f time=%.1f day=%d)\n",
                _sunElevation, _sunAzimuth, _latitude, _longitude, _timeOfDay, _dayOfYear);
        return 1;
    }

    // Quality preset handler
    if (k->is("vdb_quality_preset")) {
        // Custom(0), Draft(1), Preview(2), Production(3), Final(4), Ultra(5)
        struct QPreset { double q; int sh; double shDen; double envD; };
        static const QPreset qv[] = {
            {},                          // 0: Custom
            {1.5,  4, 1.0, 0.3},        // 1: Draft
            {3.0,  8, 1.0, 0.4},        // 2: Preview
            {5.0, 16, 1.0, 0.6},        // 3: Production
            {7.0, 24, 1.0, 0.7},        // 4: Final
            {10.0,32, 1.0, 1.0},        // 5: Ultra
        };
        if (_vdbQualityPreset > 0 && _vdbQualityPreset < 6) {
            const auto& q = qv[_vdbQualityPreset];
            _vdbQuality = q.q;
            _vdbShadowSteps = q.sh;
            _vdbShadowDensity = q.shDen;
            _vdbEnvDiffuse = q.envD;
            if (knob("vdb_quality")) knob("vdb_quality")->set_value(q.q);
            if (knob("vdb_shadow_steps")) knob("vdb_shadow_steps")->set_value(q.sh);
            if (knob("vdb_shadow_density")) knob("vdb_shadow_density")->set_value(q.shDen);
            if (knob("vdb_env_diffuse")) knob("vdb_env_diffuse")->set_value(q.envD);
        }
        return 1;
    }

    // Any quality/shadow/env knob change resets preset to Custom
    if (k->is("vdb_quality") || k->is("vdb_shadow_steps") || k->is("vdb_shadow_density") ||
        k->is("vdb_env_diffuse")) {
        if (knob("vdb_quality_preset")) { knob("vdb_quality_preset")->set_value(0); _vdbQualityPreset = 0; }
        return 1;
    }

    // Axis control buttons (disabled — use GeoTransform upstream instead)
    /*
    if (k->is("vdb_rot_x90")) { _vdbRotate[0] = std::fmod(_vdbRotate[0]+90.0, 360.0); if(knob("vdb_rotate_x")) knob("vdb_rotate_x")->set_value(_vdbRotate[0]); _vdbPreviewDirty=true; return 1; }
    if (k->is("vdb_rot_y90")) { _vdbRotate[1] = std::fmod(_vdbRotate[1]+90.0, 360.0); if(knob("vdb_rotate_y")) knob("vdb_rotate_y")->set_value(_vdbRotate[1]); _vdbPreviewDirty=true; return 1; }
    if (k->is("vdb_rot_z90")) { _vdbRotate[2] = std::fmod(_vdbRotate[2]+90.0, 360.0); if(knob("vdb_rotate_z")) knob("vdb_rotate_z")->set_value(_vdbRotate[2]); _vdbPreviewDirty=true; return 1; }
    if (k->is("vdb_mirror_x")) { _vdbScale[0] = -_vdbScale[0]; if(knob("vdb_scale_x")) knob("vdb_scale_x")->set_value(_vdbScale[0]); _vdbPreviewDirty=true; return 1; }
    if (k->is("vdb_mirror_y")) { _vdbScale[1] = -_vdbScale[1]; if(knob("vdb_scale_y")) knob("vdb_scale_y")->set_value(_vdbScale[1]); _vdbPreviewDirty=true; return 1; }
    if (k->is("vdb_mirror_z")) { _vdbScale[2] = -_vdbScale[2]; if(knob("vdb_scale_z")) knob("vdb_scale_z")->set_value(_vdbScale[2]); _vdbPreviewDirty=true; return 1; }
    if (k->is("vdb_reset_xform")) {
        _vdbTranslate[0]=_vdbTranslate[1]=_vdbTranslate[2]=0;
        _vdbRotate[0]=_vdbRotate[1]=_vdbRotate[2]=0;
        _vdbScale[0]=_vdbScale[1]=_vdbScale[2]=1; _vdbUniformScale=1;
        if(knob("vdb_translate_x")) knob("vdb_translate_x")->set_value(0);
        if(knob("vdb_translate_y")) knob("vdb_translate_y")->set_value(0);
        if(knob("vdb_translate_z")) knob("vdb_translate_z")->set_value(0);
        if(knob("vdb_rotate_x")) knob("vdb_rotate_x")->set_value(0);
        if(knob("vdb_rotate_y")) knob("vdb_rotate_y")->set_value(0);
        if(knob("vdb_rotate_z")) knob("vdb_rotate_z")->set_value(0);
        if(knob("vdb_scale_x")) knob("vdb_scale_x")->set_value(1);
        if(knob("vdb_scale_y")) knob("vdb_scale_y")->set_value(1);
        if(knob("vdb_scale_z")) knob("vdb_scale_z")->set_value(1);
        if(knob("vdb_uniform_scale")) knob("vdb_uniform_scale")->set_value(1);
        _vdbPreviewDirty=true; return 1;
    }
    if (k->is("vdb_translate_x") || k->is("vdb_translate_y") || k->is("vdb_translate_z") ||
        k->is("vdb_rotate_x") || k->is("vdb_rotate_y") || k->is("vdb_rotate_z") ||
        k->is("vdb_scale_x") || k->is("vdb_scale_y") || k->is("vdb_scale_z") ||
        k->is("vdb_uniform_scale")) {
        _vdbPreviewDirty = true;
        return 1;
    }
    */

    if (k->is("vdb_render_mode") || k->is("vdb_intensity") || k->is("vdb_spectral_volumes")) return 1;
    if (k->is("vdb_vol_res")) {
        _vdbLoadedPath.clear(); _volume.reset(); _vdbIsPreviewRes = true;
        _frameReady.store(false); _sceneReady.store(false);
        return 1;
    }
    if (k->is("vdb_shadow_cache") || k->is("vdb_shadow_cache_res")) return 1;
    if (k->is("vdb_phase_mode") || k->is("vdb_mie_droplet_d") || k->is("vdb_gradient_mix")) return 1;
    if (k->is("vdb_env_intensity") || k->is("vdb_env_rotate") ||
        k->is("vdb_env_mode") || k->is("vdb_env_virtual_lights") || k->is("vdb_use_restir")) return 1;
    if (k->is("hdri_file") || k->is("hdri_intensity") || k->is("hdri_rotate")) return 1;
    if (k->is("vdb_noise_enable") || k->is("vdb_noise_scale") || k->is("vdb_noise_strength") ||
        k->is("vdb_noise_octaves") || k->is("vdb_noise_roughness") || k->is("vdb_noise_normalize")) return 1;

    // Shading preset handler
    if (k->is("vdb_shading_preset") && _vdbShadingPreset > 0) {
        struct SPreset { double ext; double scat; double dens; double gF; double gB; double lM;
                         double pow; double emI; double tMin; double tMax; double flI;
                         float scR; float scG; float scB; int rmode; double inten;
                         int phase; double mieD; };
        //                             ext  scat dens  gF    gB    lM   pow emI  tMin  tMax  flI   scR   scG   scB  rm  int  ph  mie
        static const SPreset sp[] = {
            {},                                                                                                                   // 0: Custom
            // -- Smoke -- (HG phase, no Mie)
            {3,   1.5, 1,   0.3, -0.1,  0.8,  0,  0,    0,    0,    0,  0.7f, 0.7f, 0.7f,  0, 1,   0, 0},   // 1: Light Smoke
            {8,   3,   1.5, 0.25,-0.15, 0.75, 0,  0,    0,    0,    0,  0.5f, 0.5f, 0.5f,  0, 1,   0, 0},   // 2: Dense Smoke
            {12,  4,   2,   0.2, -0.1,  0.7,  0,  0,    0,    0,    0,  0.4f, 0.38f,0.35f, 0, 1,   0, 0},   // 3: Industrial
            // -- Fire -- (HG phase)
            {2,   1.5, 1,   0.4, -0.15, 0.7,  1,  5,  800, 2500,   8,  1.f,  0.9f, 0.7f,  5, 1.5, 0, 0},   // 4: Campfire
            {4,   3,   1.2, 0.6, -0.25, 0.75, 3, 10,  300, 5000,  15,  1.f,  0.85f,0.6f,  5, 2,   0, 0},   // 5: Explosion
            {6,   4,   1.5, 0.7, -0.3,  0.8,  4, 15,  200, 8000,  20,  0.9f, 0.8f, 0.6f,  5, 2.5, 0, 0},   // 6: Pyroclastic
            // -- Clouds -- (Mie phase, water droplets)
            {2.5, 2.4, 1,   0.85,-0.1,  0.85, 3,  0,    0,    0,    0,  1.f,  1.f,  1.f,   0, 2,   1, 8},   // 7: Cumulus (large drops)
            {0.4, 0.38,0.6, 0.75,-0.05, 0.9,  1,  0,    0,    0,    0,  1.f,  1.f,  1.f,   0, 3,   1, 1},   // 8: Cirrus (ice crystals, small)
            {3.5, 3.4, 1,   0.85,-0.1,  0.85, 2,  0,    0,    0,    0,  0.98f,0.98f,1.f,   0, 2,   1, 5},   // 9: Stratus (mid droplets)
            {6,   5.7, 1.5, 0.87,-0.25, 0.8,  4,  0,    0,    0,    0,  0.9f, 0.9f, 0.92f, 0, 1.5, 1, 10},  // 10: Storm (large drops)
            // -- Atmosphere -- (Mie for fog/mist/haze, HG for dust)
            {1.5, 1.4, 0.5, 0.6, -0.15, 0.85, 0,  0,    0,    0,    0,  0.92f,0.94f,0.97f, 0, 1.5, 1, 3},   // 11: Fog (water droplets)
            {0.8, 0.75,0.3, 0.5, -0.1,  0.85, 0,  0,    0,    0,    0,  0.95f,0.96f,1.f,   0, 2,   1, 2},   // 12: Ground Mist (fine drops)
            {0.3, 0.28,0.2, 0.7, -0.05, 0.9,  0,  0,    0,    0,    0,  0.9f, 0.92f,0.98f, 0, 3,   1, 0.5}, // 13: Haze (aerosol)
            {5,   2,   1.5, 0.4, -0.1,  0.6,  0,  0,    0,    0,    0,  0.85f,0.75f,0.6f,  0, 1,   0, 0},   // 14: Dust Storm (HG)
            // -- Effects --
            {0.8, 0.4, 0.8, 0.2,  0,    1,    0,  5, 2000,15000,   0,  0.6f, 0.4f, 1.f,   0, 2,   0, 0},   // 15: Nebula
            {0.5, 0.48,0.4, 0.85,-0.1,  0.9,  1,  0,    0,    0,    0,  0.7f, 0.9f, 1.f,   0, 2,   1, 4},   // 16: Underwater (water)
        };
        int presetCount = sizeof(sp) / sizeof(sp[0]);
        if (_vdbShadingPreset < presetCount) {
            const auto& s = sp[_vdbShadingPreset];
            _vdbExtinction=s.ext; _vdbScattering=s.scat; _vdbDensityMult=s.dens;
            _vdbGForward=s.gF; _vdbGBackward=s.gB; _vdbLobeMix=s.lM;
            _vdbPowder=s.pow; _vdbEmissionIntensity=s.emI;
            _vdbTempMin=s.tMin; _vdbTempMax=s.tMax; _vdbFlameIntensity=s.flI;
            _vdbScatterColor[0]=s.scR; _vdbScatterColor[1]=s.scG; _vdbScatterColor[2]=s.scB;
            _vdbIntensity=s.inten; _vdbPhaseMode=s.phase; _vdbMieDropletD=s.mieD;
            if (knob("vdb_extinction"))   knob("vdb_extinction")->set_value(s.ext);
            if (knob("vdb_scattering"))   knob("vdb_scattering")->set_value(s.scat);
            if (knob("vdb_density_mult")) knob("vdb_density_mult")->set_value(s.dens);
            if (knob("vdb_g_forward"))    knob("vdb_g_forward")->set_value(s.gF);
            if (knob("vdb_g_backward"))   knob("vdb_g_backward")->set_value(s.gB);
            if (knob("vdb_lobe_mix"))     knob("vdb_lobe_mix")->set_value(s.lM);
            if (knob("vdb_powder"))       knob("vdb_powder")->set_value(s.pow);
            if (knob("vdb_emission_intensity")) knob("vdb_emission_intensity")->set_value(s.emI);
            if (knob("vdb_temp_min"))     knob("vdb_temp_min")->set_value(s.tMin);
            if (knob("vdb_temp_max"))     knob("vdb_temp_max")->set_value(s.tMax);
            if (knob("vdb_flame_intensity")) knob("vdb_flame_intensity")->set_value(s.flI);
            if (knob("vdb_intensity"))    knob("vdb_intensity")->set_value(s.inten);
            if (knob("vdb_scatter_color")) {
                knob("vdb_scatter_color")->set_value(s.scR, 0);
                knob("vdb_scatter_color")->set_value(s.scG, 1);
                knob("vdb_scatter_color")->set_value(s.scB, 2);
            }
            // Set render mode
            if (knob("vdb_render_mode")) {
                knob("vdb_render_mode")->set_value(s.rmode); _vdbRenderMode = s.rmode;
            }
            // Set phase function mode
            if (knob("vdb_phase_mode")) {
                knob("vdb_phase_mode")->set_value(s.phase); _vdbPhaseMode = s.phase;
            }
            if (s.mieD > 0 && knob("vdb_mie_droplet_d")) {
                knob("vdb_mie_droplet_d")->set_value(s.mieD); _vdbMieDropletD = s.mieD;
            }
        }
        return 1;
    }

    if (k->is("rerender")) {
        // Cancel async render
        _asyncCancel.store(true);
        if (_asyncQualityThread.joinable()) _asyncQualityThread.join();

        // Flush all caches
        _frameReady.store(false);
        _sceneReady.store(false);
        _progressiveSppDone = 0;
        _scene.reset();
        _volume.reset();
        _volumes.clear();
        _vdbLastLoadedFrame = -999;

        // Force GPU to rebuild everything
#ifdef SPECTRAL_HAS_OPTIX
        SpectralIntegrator::InvalidateGPUAccel();
#endif

        SLOG("SpectralRender: re-render — all caches flushed\n");
        invalidate();
        return 1;
    }

    return Iop::knob_changed(k);
}

// ---------------------------------------------------------------------------
// _validate
// ---------------------------------------------------------------------------
void SpectralRenderIop::_validate(bool forReal)
{
    // Read current frame from Nuke's timeline
    int currentFrame = static_cast<int>(outputContext().frame());
    if (currentFrame != _frame) {
        _asyncCancel.store(true);
        if (_asyncQualityThread.joinable()) _asyncQualityThread.join();
        _frame = currentFrame;
        _frameReady.store(false); _sceneReady.store(false);
        _progressiveSppDone = 0;
        _vdbLastLoadedFrame = -999;
        _volume.reset();
        _volumes.clear();
    }

    // Always invalidate render when Nuke re-validates (upstream input may have changed)
    if (forReal) {
        _frameReady.store(false);
        _sceneReady.store(false);
    }
    // If we were scrubbing and have now stopped, force re-render
    // (scrub detection removed — preview-res loading handles performance)
    // ------------------------------------------------------------------
    // Determine output format:
    //   Priority: BG input format > format knob > fallback
    // ------------------------------------------------------------------
    const Format* fmtPtr = nullptr;

    // Try BG input (input 2) first
    Op* bgOp = (inputs() > 2) ? input(2) : nullptr;
    Iop* bgIop = bgOp ? dynamic_cast<Iop*>(bgOp) : nullptr;
    if (bgIop) {
        try {
            bgIop->validate(forReal);
            fmtPtr = &(bgIop->info().format());
        } catch (...) {
            bgIop = nullptr;
        }
    }

    // Fall back to format knob
    if (!fmtPtr) {
        fmtPtr = _outputFormat.format();
    }

    if (!fmtPtr) {
        info_.channels(Mask_RGBA);
        return;
    }

    info_.format(*fmtPtr);
    info_.full_size_format(*fmtPtr);
    info_.set(*fmtPtr);

    // Register channels: RGBA + custom ID channels
    ChannelSet channels = Mask_RGBA;
    _chanObjectId   = getChannel("other.objectId");
    _chanMaterialId = getChannel("other.materialId");
    _chanAO         = getChannel("other.ao");
    channels += _chanObjectId;
    channels += _chanMaterialId;
    if (_aoSamples > 0) channels += _chanAO;

    if (_aovNormals) {
        _chanNx = getChannel("N.red"); _chanNy = getChannel("N.green"); _chanNz = getChannel("N.blue");
        channels += _chanNx; channels += _chanNy; channels += _chanNz;
    }
    if (_aovPosition) {
        _chanPx = getChannel("P.red"); _chanPy = getChannel("P.green"); _chanPz = getChannel("P.blue");
        channels += _chanPx; channels += _chanPy; channels += _chanPz;
    }
    if (_aovPRef) {
        _chanPRefX = getChannel("pRef.red"); _chanPRefY = getChannel("pRef.green"); _chanPRefZ = getChannel("pRef.blue");
        channels += _chanPRefX; channels += _chanPRefY; channels += _chanPRefZ;
    }
    if (_aovUV) {
        _chanUu = getChannel("uv.red"); _chanUv = getChannel("uv.green");
        channels += _chanUu; channels += _chanUv;
    }
    if (_aovAlbedo) {
        _chanAlbedoR = getChannel("albedo.red"); _chanAlbedoG = getChannel("albedo.green"); _chanAlbedoB = getChannel("albedo.blue");
        channels += _chanAlbedoR; channels += _chanAlbedoG; channels += _chanAlbedoB;
    }
    if (_aovDepth) {
        // Nuke's depth.Z convention -- ZDefocus / ZBlur / ZMerge all read this.
        _chanDepth = getChannel("depth.Z");
        channels += _chanDepth;
    }
    if (_aovDirect) {
        _chanDirectR = getChannel("direct.red"); _chanDirectG = getChannel("direct.green"); _chanDirectB = getChannel("direct.blue");
        channels += _chanDirectR; channels += _chanDirectG; channels += _chanDirectB;
    }
    if (_aovIndirect) {
        _chanIndirectR = getChannel("indirect.red"); _chanIndirectG = getChannel("indirect.green"); _chanIndirectB = getChannel("indirect.blue");
        channels += _chanIndirectR; channels += _chanIndirectG; channels += _chanIndirectB;
    }
    if (_aovEmission) {
        _chanEmissionR = getChannel("emission.red"); _chanEmissionG = getChannel("emission.green"); _chanEmissionB = getChannel("emission.blue");
        channels += _chanEmissionR; channels += _chanEmissionG; channels += _chanEmissionB;
    }
    if (_aovDiffuseDirect) {
        _chanDiffDirectR = getChannel("diffuse_direct.red"); _chanDiffDirectG = getChannel("diffuse_direct.green"); _chanDiffDirectB = getChannel("diffuse_direct.blue");
        channels += _chanDiffDirectR; channels += _chanDiffDirectG; channels += _chanDiffDirectB;
    }
    if (_aovSpecularDirect) {
        _chanSpecDirectR = getChannel("specular_direct.red"); _chanSpecDirectG = getChannel("specular_direct.green"); _chanSpecDirectB = getChannel("specular_direct.blue");
        channels += _chanSpecDirectR; channels += _chanSpecDirectG; channels += _chanSpecDirectB;
    }
    if (_aovDiffuseIndirect) {
        _chanDiffIndirectR = getChannel("diffuse_indirect.red"); _chanDiffIndirectG = getChannel("diffuse_indirect.green"); _chanDiffIndirectB = getChannel("diffuse_indirect.blue");
        channels += _chanDiffIndirectR; channels += _chanDiffIndirectG; channels += _chanDiffIndirectB;
    }
    if (_aovSpecularIndirect) {
        _chanSpecIndirectR = getChannel("specular_indirect.red"); _chanSpecIndirectG = getChannel("specular_indirect.green"); _chanSpecIndirectB = getChannel("specular_indirect.blue");
        channels += _chanSpecIndirectR; channels += _chanSpecIndirectG; channels += _chanSpecIndirectB;
    }
    if (_aovTransmission) {
        _chanTransmitR = getChannel("transmission.red"); _chanTransmitG = getChannel("transmission.green"); _chanTransmitB = getChannel("transmission.blue");
        channels += _chanTransmitR; channels += _chanTransmitG; channels += _chanTransmitB;
    }
    // Cryptomatte: Nuke gizmo expects crypto_object00 RGBA
    _chanCryptoR = getChannel("crypto_object00.red");
    _chanCryptoG = getChannel("crypto_object00.green");
    _chanCryptoB = getChannel("crypto_object00.blue");
    _chanCryptoA = getChannel("crypto_object00.alpha");
    channels += _chanCryptoR; channels += _chanCryptoG;
    channels += _chanCryptoB; channels += _chanCryptoA;

    info_.channels(channels);

    // Cryptomatte metadata — required for Nuke's Cryptomatte gizmo
    {
        _cryptoMeta.setData("exr/cryptomatte/a1b2c3/name", "crypto_object");
        _cryptoMeta.setData("exr/cryptomatte/a1b2c3/hash", "MurmurHash3_32");
        _cryptoMeta.setData("exr/cryptomatte/a1b2c3/conversion", "uint32_to_float32");
        _cryptoMeta.setData("exr/cryptomatte/a1b2c3/manifest", "{}");
        _cryptoMetaReady = true;
    }
    info_.black_outside(true);

    // Check if scn input changed (GeoTransform, VDBRead knobs, etc.)
    Op* scnOp = (inputs() > 1) ? input(1) : nullptr;
    bool scnChanged = false;
    if (scnOp) {
        scnOp->validate(forReal);
        Hash newHash;
        newHash.append(scnOp->hash());
        // Include ShaderOp hashes (walk inputs to catch material changes)
        std::vector<Op*> walkOps;
        walkOps.push_back(scnOp);
        for (int w = 0; w < (int)walkOps.size() && w < 32; ++w) {
            Op* cur = walkOps[w];
            if (!cur) continue;
            newHash.append(cur->hash());
            for (int inp = 0; inp < cur->inputs() && inp < 8; ++inp)
                if (cur->input(inp)) walkOps.push_back(cur->input(inp));
        }
        // Include SpectralSurface registry
        const auto& reg = SpectralSurfaceOp::GetRegistry();
        for (const auto& kv : reg) {
            newHash.append(kv.first.c_str());
            newHash.append(kv.second.metalType);
            newHash.append(kv.second.abbeNumber);
            newHash.append(kv.second.textureBlend);
        }
        // Include global version counter
        extern int GetSpectralSurfaceVersion();
        newHash.append(GetSpectralSurfaceVersion());
        if (newHash != _scnInputHash) {
            _scnInputHash = newHash;
            _vdbLastLoadedFrame = -999;  // force VDB reload
            _frameReady.store(false); _sceneReady.store(false);    // force re-render
            _vdbPreviewDirty = true;
            _vdbPreviewPoints.clear();
            scnChanged = true;
        }
    } else if (_scnInputHash != Hash()) {
        // scn input was connected, now disconnected — clear volumes
        _scnInputHash = Hash();
        _volumes.clear();
        _volume.reset();
        _cachedVolMerge = nullptr;
        _hasRefVolCenter = false;
        _vdbLastLoadedFrame = -999;
        _frameReady.store(false); _sceneReady.store(false);
        _vdbPreviewDirty = true;
        _vdbPreviewPoints.clear();
        scnChanged = true;
    }

    if (forReal || scnChanged) {
        _asyncCancel.store(true);
        if (_asyncQualityThread.joinable()) _asyncQualityThread.join();
        _LoadStage();
        _BuildCameraFromInput();
        _frameReady.store(false); _sceneReady.store(false);
        _progressiveSppDone = 0;
    }

    // Load VDB for viewport preview — cached per frame, only reloads when frame changes
    _LoadVDB();

    // Deep output info: construct from flat IopInfo, which copies format/bbox/channels.
    // The IopInfo-based DeepInfo constructor handles everything.
    _deepInfo = DeepInfo(info_);
}

void SpectralRenderIop::_request(int x, int y, int r, int t,
                                  ChannelMask channels, int count)
{
    // Request BG input pixels if connected
    Op* bgOp = (inputs() > 2) ? input(2) : nullptr;
    Iop* bgIop = bgOp ? dynamic_cast<Iop*>(bgOp) : nullptr;
    if (bgIop) {
        bgIop->request(x, y, r, t, channels, count);
    }
}

// ---------------------------------------------------------------------------
// engine — render pixels, composite BG behind
// ---------------------------------------------------------------------------
void SpectralRenderIop::engine(
    int y, int x, int r, ChannelMask channels, Row& row)
{
    _EnsureSceneReady();

    const int W = static_cast<int>(_fbWidth);       // proxy render resolution
    const int H = static_cast<int>(_fbHeight);
    const int fullW = static_cast<int>(_fbFullWidth ? _fbFullWidth : _fbWidth);
    const int fullH = static_cast<int>(_fbFullHeight ? _fbFullHeight : _fbHeight);

    int fullBufY = fullH - 1 - y;
    // Map output Y to proxy buffer Y
    int bufY = (H == fullH) ? fullBufY : (fullBufY * H / fullH);

    // Progressive strip rendering: render this strip on demand
    if (bufY >= 0 && bufY < H && !_stripRendered.empty()) {
        int stripIdx = bufY / kStripHeight;
        int numStrips = static_cast<int>(_stripRendered.size());
        if (stripIdx < numStrips && !_stripRendered[stripIdx]) {
            _RenderStrip(stripIdx);
        }
    }

    // Get BG pixels if available
    Row bgRow(x, r);
    Op* bgOp = (inputs() > 2) ? input(2) : nullptr;
    Iop* bgIop = bgOp ? dynamic_cast<Iop*>(bgOp) : nullptr;
    bool hasBG = false;
    if (bgIop) {
        bgIop->get(y, x, r, channels, bgRow);
        hasBG = true;
    }

    if (bufY < 0 || bufY >= H || _frameBuffer.empty()) {
        // Outside render area — pass through BG or black
        if (hasBG) {
            foreach(z, channels) {
                const float* bgPtr = bgRow[z] + x;
                float* out = row.writable(z) + x;
                for (int px = x; px < r; ++px)
                    *out++ = *bgPtr++;
            }
        } else {
            row.erase(channels);
        }
        return;
    }

    const float* srcRow = _frameBuffer.data() + bufY * W * 4;
    const bool isProxy = (W != fullW);

    // Composite: render over BG using render alpha
    auto compositeChannel = [&](Channel ch, int comp) {
        if (!(channels & ch)) return;
        float* out = row.writable(ch);
        const float* bgPtr = hasBG ? bgRow[ch] + x : nullptr;

        for (int px = x; px < r; ++px) {
            int proxyX = isProxy ? (px * W / fullW) : px;
            float fg = 0.f, fgA = 0.f;
            if (proxyX >= 0 && proxyX < W) {
                fg  = srcRow[proxyX * 4 + comp];
                fgA = srcRow[proxyX * 4 + 3];
            }

            if (bgPtr) {
                float bg = bgPtr[px - x];
                out[px] = fg + bg * (1.f - fgA);
            } else {
                out[px] = fg;
            }
        }
    };

    compositeChannel(Chan_Red,   0);
    compositeChannel(Chan_Green, 1);
    compositeChannel(Chan_Blue,  2);

    // Alpha: render alpha over BG alpha
    if (channels & Chan_Alpha) {
        float* out = row.writable(Chan_Alpha);
        const float* bgPtr = hasBG ? bgRow[Chan_Alpha] + x : nullptr;

        for (int px = x; px < r; ++px) {
            int proxyX = isProxy ? (px * W / fullW) : px;
            float fgA = (proxyX >= 0 && proxyX < W) ? srcRow[proxyX * 4 + 3] : 0.f;
            if (bgPtr) {
                float bgA = bgPtr[px - x];
                out[px] = fgA + bgA * (1.f - fgA);
            } else {
                out[px] = fgA;
            }
        }
    }

    // Output object/material ID channels
    auto writeIdChannel = [&](Channel ch, const std::vector<float>& idBuf) {
        if (!(channels & ch) || idBuf.empty()) return;
        float* out = row.writable(ch);
        for (int px = x; px < r; ++px) {
            int proxyX = isProxy ? (px * W / fullW) : px;
            if (proxyX >= 0 && proxyX < W && bufY >= 0 && bufY < H) {
                out[px] = idBuf[static_cast<size_t>(bufY) * W + proxyX];
            } else {
                out[px] = 0.f;
            }
        }
    };
    writeIdChannel(_chanObjectId, _objectIdBuffer);
    writeIdChannel(_chanMaterialId, _materialIdBuffer);
    writeIdChannel(_chanAO, _aoBuffer);
    // _depthBuffer is a single float per pixel (camera-space Z, background
    // sentinel 1e30). Same shape as the ID buffers so it rides the same helper.
    writeIdChannel(_chanDepth, _depthBuffer);

    // Output vector AOV channels (3 floats per pixel)
    auto write3Channel = [&](Channel chR, Channel chG, Channel chB,
                             const std::vector<float>& buf, int stride) {
        if (buf.empty()) return;
        auto writeOne = [&](Channel ch, int comp) {
            if (!(channels & ch)) return;
            float* out = row.writable(ch);
            for (int px = x; px < r; ++px) {
                int proxyX = isProxy ? (px * W / fullW) : px;
                if (proxyX >= 0 && proxyX < W && bufY >= 0 && bufY < H) {
                    out[px] = buf[static_cast<size_t>(bufY) * W * stride + proxyX * stride + comp];
                } else {
                    out[px] = 0.f;
                }
            }
        };
        writeOne(chR, 0);
        writeOne(chG, 1);
        if (stride >= 3) writeOne(chB, 2);
    };

    write3Channel(_chanNx, _chanNy, _chanNz, _normalBuffer, 3);
    write3Channel(_chanPx, _chanPy, _chanPz, _posBuffer, 3);
    write3Channel(_chanPRefX, _chanPRefY, _chanPRefZ, _pRefBuffer, 3);
    write3Channel(_chanUu, _chanUv, Chan_Black, _uvBuffer, 2);
    write3Channel(_chanAlbedoR, _chanAlbedoG, _chanAlbedoB, _albedoBuffer, 3);
    write3Channel(_chanDirectR, _chanDirectG, _chanDirectB, _directBuffer, 3);
    write3Channel(_chanIndirectR, _chanIndirectG, _chanIndirectB, _indirectBuffer, 3);
    write3Channel(_chanEmissionR, _chanEmissionG, _chanEmissionB, _emissionBuffer, 3);
    write3Channel(_chanDiffDirectR, _chanDiffDirectG, _chanDiffDirectB, _diffuseDirectBuffer, 3);
    write3Channel(_chanSpecDirectR, _chanSpecDirectG, _chanSpecDirectB, _specularDirectBuffer, 3);
    write3Channel(_chanDiffIndirectR, _chanDiffIndirectG, _chanDiffIndirectB, _diffuseIndirectBuffer, 3);
    write3Channel(_chanSpecIndirectR, _chanSpecIndirectG, _chanSpecIndirectB, _specularIndirectBuffer, 3);
    write3Channel(_chanTransmitR, _chanTransmitG, _chanTransmitB, _transmissionBuffer, 3);

    // Cryptomatte: crypto_object00 RGBA
    if (!_cryptoObjectBuffer.empty()) {
        auto writeCryptoChannel = [&](Channel ch, int comp) {
            if (!(channels & ch)) return;
            float* out = row.writable(ch);
            for (int px = x; px < r; ++px) {
                int proxyX = isProxy ? (px * W / fullW) : px;
                if (proxyX >= 0 && proxyX < W && bufY >= 0 && bufY < H)
                    out[px] = _cryptoObjectBuffer[static_cast<size_t>(bufY) * W * 4 + proxyX * 4 + comp];
                else out[px] = 0.f;
            }
        };
        writeCryptoChannel(_chanCryptoR, 0);
        writeCryptoChannel(_chanCryptoG, 1);
        writeCryptoChannel(_chanCryptoB, 2);
        writeCryptoChannel(_chanCryptoA, 3);
    }
}

// ---------------------------------------------------------------------------
// _LoadStage — try GeomOp input first, then file knob fallback
// ---------------------------------------------------------------------------
void SpectralRenderIop::_LoadStage()
{
    _scene = std::make_unique<pxr::SpectralScene>();
    _projCameraVP.clear();
    _shadowCatcherMatIds.clear();
    _noShadowCastMatIds.clear();
    _camera = SpectralCamera();
    _vdbHasSceneXform = false;
    _volumeXforms.clear();

    // ------------------------------------------------------------------
    // Path 1: GeomOp input connected — use Nuke's USD scene graph
    // ------------------------------------------------------------------
    Op* in0 = (maximum_inputs() > 1 && inputs() > 1) ? input(1) : nullptr;
    GeometryProviderI* geoProvider = in0 ? dynamic_cast<GeometryProviderI*>(in0) : nullptr;

    if (geoProvider) {
        SLOG("SpectralRender: reading from node graph input\n");

        try {
            // Validate the upstream op
            in0->validate(true);

            // Build a usg::Stage from the upstream GeomOp.
            //
            // GeometryProviderI::BuildStage evaluates the Nuke op graph at
            // a single time and bakes the result into a static USD stage.
            // Earlier we tried building two stages at tOpen and tClose to
            // detect motion, plus Op::setOutputContext + invalidate +
            // validate to force the chain to re-evaluate -- none of it
            // worked. The USG bridge bakes at Nuke's global current frame
            // regardless of what we do. Diagnostics confirmed both stages
            // came out identical.
            //
            // What DOES work: walking the input chain and reading knobs
            // directly via Knob::get_value_at(time). That's handled inside
            // _LoadFromPxrStage via the nodeGraphInput pointer; see the
            // chain-walk helpers above this function.
            usg::StageRef usgStage = usg::Stage::CreateInMemory();
            if (!usgStage) {
                SLOG("SpectralRender: failed to create in-memory stage\n");
                return;
            }

            fdk::TimeValue sampleTime(static_cast<double>(_frame));
            OpGraphLocation location(in0);
            GeometryProviderI::BuildStage(usgStage, location, sampleTime);

            if (!usgStage || !usgStage->isValid()) {
                SLOG("SpectralRender: BuildStage produced invalid stage\n");
                return;
            }

            // Extract the PXR UsdStageRefPtr from the usg::Stage
            int usdVer = usg::usdAPIVersion();
            usg::Stage::Handle* handle = usgStage->getUsdStageRefPtr(usdVer);
            if (!handle) {
                SLOG("SpectralRender: getUsdStageRefPtr failed (version mismatch?)\n");
                return;
            }

            // Cast to PXR UsdStageRefPtr
            UsdStageRefPtr* pxrStagePtr = reinterpret_cast<UsdStageRefPtr*>(handle);
            if (!pxrStagePtr || !(*pxrStagePtr)) {
                SLOG("SpectralRender: PXR stage pointer is null\n");
                return;
            }

            const bool nodeGraphMotionBlur =
                _objectMotionBlur && (_shutterOpen != _shutterClose);
            const double tOpenLog  = static_cast<double>(_frame) + _shutterOpen;
            const double tCloseLog = static_cast<double>(_frame) + _shutterClose;

            SLOG("SpectralRender: got PXR stage from node graph%s\n",
                 nodeGraphMotionBlur ? " (chain-walk motion blur enabled)" : "");

            // Input chain diagnostic: log any transform-like knobs we find,
            // reading open/close values directly. Useful for verifying what
            // the chain walker will see. Kept in place because motion blur
            // bugs are hard to debug without it.
            if (nodeGraphMotionBlur) {
                SLOG("SpectralRender: motion blur stage times -- open=%g close=%g (frame=%d)\n",
                     tOpenLog, tCloseLog, _frame);
                Op* cur = in0;
                for (int d = 0; d < 8 && cur; ++d) {
                    const char* cls = cur->Class();
                    DD::Image::Knob* kTrans = cur->knob("translate");
                    DD::Image::Knob* kRot   = cur->knob("rotate");
                    DD::Image::Knob* kScale = cur->knob("scaling");
                    DD::Image::Knob* kUScl  = cur->knob("uniform_scale");
                    if (kTrans || kRot || kScale || kUScl) {
                        SLOG("SpectralRender: mblur chain[%d] class=%s",
                             d, cls ? cls : "?");
                        if (kRot) {
                            SLOG(" rot_open=(%.3f,%.3f,%.3f) rot_close=(%.3f,%.3f,%.3f)",
                                 (float)kRot->get_value_at(tOpenLog, 0),
                                 (float)kRot->get_value_at(tOpenLog, 1),
                                 (float)kRot->get_value_at(tOpenLog, 2),
                                 (float)kRot->get_value_at(tCloseLog, 0),
                                 (float)kRot->get_value_at(tCloseLog, 1),
                                 (float)kRot->get_value_at(tCloseLog, 2));
                        }
                        if (kTrans) {
                            SLOG(" tr_open=(%.3f,%.3f,%.3f) tr_close=(%.3f,%.3f,%.3f)",
                                 (float)kTrans->get_value_at(tOpenLog, 0),
                                 (float)kTrans->get_value_at(tOpenLog, 1),
                                 (float)kTrans->get_value_at(tOpenLog, 2),
                                 (float)kTrans->get_value_at(tCloseLog, 0),
                                 (float)kTrans->get_value_at(tCloseLog, 1),
                                 (float)kTrans->get_value_at(tCloseLog, 2));
                        }
                        SLOG("\n");
                    } else if (d < 3) {
                        SLOG("SpectralRender: mblur chain[%d] class=%s (no xform knobs)\n",
                             d, cls ? cls : "?");
                    }
                    cur = cur->input(0);
                }
            }

            // Pass in0 through so the mesh loader can chain-walk to get
            // correct open/close xforms. stageClose is a null stub now --
            // kept for signature back-compat but no longer used.
            _LoadFromPxrStage(*pxrStagePtr, UsdStageRefPtr(), in0);
            return;

        } catch (const std::exception& e) {
            SLOG("SpectralRender: node graph input error: %s\n", e.what());
        } catch (...) {
            SLOG("SpectralRender: node graph input unknown error\n");
        }
    }

    // ------------------------------------------------------------------
    // Path 2: File knob fallback
    // ------------------------------------------------------------------
    if (!_usdFilePath || _usdFilePath[0] == '\0') {
        SLOG("SpectralRender: no input connected and no USD file set\n");
        return;
    }

    SLOG("SpectralRender: opening file %s\n", _usdFilePath);

    UsdStageRefPtr stage;
    try {
        stage = UsdStage::Open(std::string(_usdFilePath));
    } catch (const std::exception& e) {
        SLOG("SpectralRender: exception opening stage: %s\n", e.what());
        return;
    } catch (...) {
        SLOG("SpectralRender: unknown exception opening stage\n");
        return;
    }
    if (!stage) {
        SLOG("SpectralRender: UsdStage::Open returned null\n");
        return;
    }

    SLOG("SpectralRender: stage opened OK from file\n");
    _LoadFromPxrStage(stage);
}

// ---------------------------------------------------------------------------
// Chain-walk matrix helpers.
//
// The Nuke 3D -> USD bridge (GeometryProviderI::BuildStage) bakes the stage
// at Nuke's current global frame; it ignores the sampleTime we pass it and
// doesn't respect Op::setOutputContext on our input op. So we can't trust
// the bridge to give us open-time and close-time xforms -- both builds
// return the same current-frame snapshot.
//
// Workaround: walk up the input op chain ourselves, reading transform
// knobs directly via Knob::get_value_at(time, component). That API DOES
// honour the requested time (diagnosed by logging the rotate values at
// open vs close -- they differed correctly). Compose a world matrix per
// time and use it to drive motion blur.
//
// Limitations:
//   - Only recognises translate / rotate / scaling / uniform_scale knobs.
//     Pivot and skew aren't handled yet; most real scenes don't use them.
//   - Rotation order is fixed at Nuke's default (ZXY). Ops with custom
//     rot_order will produce wrong motion matrices (shape animates
//     correctly at current frame because that part comes from the USG
//     bridge; only the motion delta is affected).
//   - If any op in the chain has a transform that our knob read can't
//     reconstruct, its motion contribution is missed.
// ---------------------------------------------------------------------------

namespace {

// Build a local 4x4 matrix from translate/rotate/scaling knobs on an op,
// sampled at 'time'. Returns identity if the op has no transform knobs.
// Matrix follows the USD row-vector convention (p_world = p_local * M).
pxr::GfMatrix4d _ExtractOpLocalMatrixAt(DD::Image::Op* op, double time)
{
    pxr::GfMatrix4d M(1.0);
    if (!op) return M;

    DD::Image::Knob* kT  = op->knob("translate");
    DD::Image::Knob* kR  = op->knob("rotate");
    DD::Image::Knob* kS  = op->knob("scaling");
    DD::Image::Knob* kUS = op->knob("uniform_scale");

    if (!kT && !kR && !kS && !kUS) return M;  // pass-through op

    pxr::GfVec3d t(0.0, 0.0, 0.0);
    pxr::GfVec3d r(0.0, 0.0, 0.0);
    pxr::GfVec3d s(1.0, 1.0, 1.0);
    double us = 1.0;
    if (kT)  for (int i = 0; i < 3; ++i) t[i] = kT->get_value_at(time, i);
    if (kR)  for (int i = 0; i < 3; ++i) r[i] = kR->get_value_at(time, i);
    if (kS)  for (int i = 0; i < 3; ++i) s[i] = kS->get_value_at(time, i);
    if (kUS) us = kUS->get_value_at(time, 0);
    s *= us;

    pxr::GfMatrix4d S(1.0); S.SetScale(s);
    pxr::GfMatrix4d T(1.0); T.SetTranslate(t);

    // Rotation: build per-axis matrices. GfRotation takes degrees.
    pxr::GfMatrix4d Rx(1.0); Rx.SetRotate(pxr::GfRotation(pxr::GfVec3d(1,0,0), r[0]));
    pxr::GfMatrix4d Ry(1.0); Ry.SetRotate(pxr::GfRotation(pxr::GfVec3d(0,1,0), r[1]));
    pxr::GfMatrix4d Rz(1.0); Rz.SetRotate(pxr::GfRotation(pxr::GfVec3d(0,0,1), r[2]));

    // Nuke default rot_order is ZXY. In row-vector convention (v * R), the
    // order of multiplication reverses relative to the Euler name: ZXY
    // means "rotate about Y first, then X, then Z" on the vector, which
    // as row-vector matrix ops is  R = Ry * Rx * Rz.
    pxr::GfMatrix4d R = Ry * Rx * Rz;

    // SRT composition in row-vector form: p' = ((p * S) * R) * T
    M = S * R * T;
    return M;
}

// Walk the input chain from 'top' upward (via input(0)) and compose the
// world matrix at 'time'. Caps depth at 16 to avoid runaway on cycles.
//
// We walk from the op closest to SpectralRender (downstream) toward the
// source (upstream). In Nuke's row-vector convention, data flow is
//   p_final = p_source * M_upstream * ... * M_downstream
// So walking downstream -> upstream, the correct accumulation is
//   world = local * world
// (local is the more-upstream op, which gets left-multiplied so the
// next iteration's even-more-upstream op ends up further left).
pxr::GfMatrix4d _ComputeInputChainWorldMatrix(DD::Image::Op* top, double time)
{
    pxr::GfMatrix4d world(1.0);
    DD::Image::Op* cur = top;
    int depth = 0;
    while (cur && depth < 16) {
        pxr::GfMatrix4d local = _ExtractOpLocalMatrixAt(cur, time);
        world = local * world;
        cur = cur->input(0);
        ++depth;
    }
    return world;
}

} // namespace

// ---------------------------------------------------------------------------
// _LoadFromPxrStage — shared mesh/camera loading from a PXR UsdStageRefPtr.
//
// nodeGraphInput, if non-null, is the Nuke input op that produced the stage.
// It enables the chain-walk motion blur path: for each mesh, we compute
// world matrices at tOpen and tClose by reading transform knobs on the
// input chain directly. This is the only way to get correct object motion
// blur from upstream animated Nuke 3D ops, because the USG bridge bakes
// its stage at a single global frame and ignores our retime attempts.
//
// For USD file input, pass nodeGraphInput = nullptr; the mesh loader then
// falls back to reading mesh.GetPointsAttr() and xforms at timeClose on
// the main stage, which works for files with native time samples.
//
// stageClose is currently unused by the mesh loader (the USG bridge
// produces identical stages regardless of sampleTime, so comparing them
// was fruitless) but the parameter is retained for signature stability.
void SpectralRenderIop::_LoadFromPxrStage(const UsdStageRefPtr& stage,
                                           const UsdStageRefPtr& stageClose,
                                           DD::Image::Op* nodeGraphInput)
{
    const UsdTimeCode timeCode(_frame);
    const UsdTimeCode timeOpen(_frame + _shutterOpen);
    const UsdTimeCode timeClose(_frame + _shutterClose);
    // Object motion blur gate: both the knob (user toggle) and the shutter
    // interval must be non-trivial. Previously this was only shutter-gated,
    // so users got object motion blur unconditionally whenever shutter was
    // non-zero (which is the default) and couldn't force reference frames
    // without editing shutter times.
    const bool motionBlurEnabled = _objectMotionBlur && (_shutterOpen != _shutterClose);
    UsdGeomXformCache xfCache(timeCode);

    int meshCount = 0;
    int totalTris = 0;
    UsdPrim cameraPrim;

    // If user specified a camera path, try that first
    if (_cameraPath && _cameraPath[0] != '\0') {
        UsdPrim p = stage->GetPrimAtPath(SdfPath(std::string(_cameraPath)));
        if (p.IsValid() && p.IsA<UsdGeomCamera>()) {
            cameraPrim = p;
            SLOG("SpectralRender: using specified camera: %s\n", _cameraPath);
        } else {
            SLOG("SpectralRender: camera not found at %s\n", _cameraPath);
        }
    }

    for (const UsdPrim& prim : stage->Traverse()) {
        // Auto-find first camera
        if (!cameraPrim.IsValid() && prim.IsA<UsdGeomCamera>()) {
            cameraPrim = prim;
            SLOG("SpectralRender: auto-found camera: %s\n",
                    prim.GetPath().GetText());
        }

        // ----------------------------------------------------------
        // Process lights (Phase 4c)
        // ----------------------------------------------------------
        if (prim.HasAPI<UsdLuxLightAPI>()) {
            UsdLuxLightAPI lightAPI(prim);
            SpectralLight light;
            light.name = prim.GetPath().GetString();

            // Determine light type
            if (prim.IsA<UsdLuxDistantLight>()) {
                light.type = SpectralLight::Type::Distant;
                // Direction from transform: distant light points down its -Z axis
                GfMatrix4d xf = xfCache.GetLocalToWorldTransform(prim);
                GfVec3d fwd = xf.TransformDir(GfVec3d(0, 0, -1));
                fwd.Normalize();
                light.direction = GfVec3f(fwd);
            } else if (prim.IsA<UsdLuxSphereLight>()) {
                light.type = SpectralLight::Type::Sphere;
                GfMatrix4d xf = xfCache.GetLocalToWorldTransform(prim);
                light.position = GfVec3f(xf.ExtractTranslation());
                UsdLuxSphereLight sph(prim);
                float r = 0.5f;
                sph.GetRadiusAttr().Get(&r, timeCode);
                // Check "treat as point" — forces radius to 0 for hard shadows
                bool treatAsPoint = false;
                sph.GetTreatAsPointAttr().Get(&treatAsPoint, timeCode);
                light.radius = treatAsPoint ? 0.f : r;

                // Check for shaping cone (SphereLight acting as SpotLight)
                VtValue coneVal;
                if (prim.GetAttribute(TfToken("inputs:shaping:cone:angle")).Get(&coneVal, timeCode)
                    && coneVal.IsHolding<float>()) {
                    float halfAngle = coneVal.UncheckedGet<float>();
                    if (halfAngle > 0.f && halfAngle < 180.f) {
                        light.type = SpectralLight::Type::Spot;
                        GfVec3d fwd = xf.TransformDir(GfVec3d(0, 0, -1));
                        fwd.Normalize();
                        light.direction = GfVec3f(fwd);
                        light.coneAngle = halfAngle * 2.f;
                        VtValue softVal;
                        if (prim.GetAttribute(TfToken("inputs:shaping:cone:softness")).Get(&softVal, timeCode)
                            && softVal.IsHolding<float>())
                            light.coneSoftness = softVal.UncheckedGet<float>();
                        light.PrecomputeCone();
                    }
                }
            } else if (prim.GetTypeName() == TfToken("SpotLight")) {
                // SpotLight: position + direction + cone
                light.type = SpectralLight::Type::Spot;
                GfMatrix4d xf = xfCache.GetLocalToWorldTransform(prim);
                light.position = GfVec3f(xf.ExtractTranslation());
                GfVec3d fwd = xf.TransformDir(GfVec3d(0, 0, -1));
                fwd.Normalize();
                light.direction = GfVec3f(fwd);
                // Read cone angle and softness
                VtValue v;
                if (prim.GetAttribute(TfToken("inputs:shaping:cone:angle")).Get(&v, timeCode)
                    && v.IsHolding<float>())
                    light.coneAngle = v.UncheckedGet<float>() * 2.f;  // USD stores half-angle
                if (prim.GetAttribute(TfToken("inputs:shaping:cone:softness")).Get(&v, timeCode)
                    && v.IsHolding<float>())
                    light.coneSoftness = v.UncheckedGet<float>();
                // Read radius for soft shadows
                if (prim.GetAttribute(TfToken("inputs:radius")).Get(&v, timeCode)
                    && v.IsHolding<float>())
                    light.radius = v.UncheckedGet<float>();
                else
                    light.radius = 0.f;
                light.PrecomputeCone();
            } else if (prim.IsA<UsdLuxRectLight>()) {
                light.type = SpectralLight::Type::Rect;
                GfMatrix4d xf = xfCache.GetLocalToWorldTransform(prim);
                light.position = GfVec3f(xf.ExtractTranslation());
                GfVec3d fwd = xf.TransformDir(GfVec3d(0, 0, -1));
                fwd.Normalize();
                light.direction = GfVec3f(fwd);
                UsdLuxRectLight rect(prim);
                float w = 1.f, h = 1.f;
                rect.GetWidthAttr().Get(&w, timeCode);
                rect.GetHeightAttr().Get(&h, timeCode);
                light.width = w;
                light.height = h;
            } else if (prim.IsA<UsdLuxDomeLight>()) {
                light.type = SpectralLight::Type::Dome;
                // Try to load HDRI texture — multiple fallback paths
                UsdLuxDomeLight dome(prim);
                std::string hdriPath;

                // Method 1: Standard UsdLux texture:file attribute
                {
                    SdfAssetPath texPath;
                    if (dome.GetTextureFileAttr().Get(&texPath, timeCode)) {
                        hdriPath = texPath.GetResolvedPath();
                        if (hdriPath.empty()) hdriPath = texPath.GetAssetPath();
                        if (!hdriPath.empty())
                            SLOG("SpectralRender: dome HDRI via GetTextureFileAttr: '%s'\n", hdriPath.c_str());
                    }
                }

                // Method 2: Search ALL attributes for any SdfAssetPath
                if (hdriPath.empty()) {
                    for (const auto& attr : prim.GetAttributes()) {
                        VtValue val;
                        if (attr.Get(&val, timeCode)) {
                            if (val.IsHolding<SdfAssetPath>()) {
                                SdfAssetPath ap = val.UncheckedGet<SdfAssetPath>();
                                std::string p = ap.GetResolvedPath();
                                if (p.empty()) p = ap.GetAssetPath();
                                SLOG("SpectralRender: dome attr '%s' = asset '%s'\n",
                                        attr.GetName().GetText(), p.c_str());
                                if (!p.empty() && hdriPath.empty()) hdriPath = p;
                            } else if (val.IsHolding<std::string>()) {
                                std::string s = val.UncheckedGet<std::string>();
                                // Check if it looks like a file path
                                if (s.find(".hdr") != std::string::npos ||
                                    s.find(".exr") != std::string::npos ||
                                    s.find(".hdri") != std::string::npos ||
                                    s.find(".tx") != std::string::npos) {
                                    SLOG("SpectralRender: dome attr '%s' = string path '%s'\n",
                                            attr.GetName().GetText(), s.c_str());
                                    if (hdriPath.empty()) hdriPath = s;
                                }
                            }
                        }
                    }
                }

                // Method 3: Check connected shader inputs (Nuke may use shader networks)
                if (hdriPath.empty()) {
                    for (const auto& rel : prim.GetRelationships()) {
                        SdfPathVector targets;
                        rel.GetTargets(&targets);
                        for (const auto& target : targets) {
                            UsdPrim targetPrim = prim.GetStage()->GetPrimAtPath(target);
                            if (targetPrim) {
                                for (const auto& attr : targetPrim.GetAttributes()) {
                                    VtValue val;
                                    if (attr.Get(&val, timeCode) && val.IsHolding<SdfAssetPath>()) {
                                        SdfAssetPath ap = val.UncheckedGet<SdfAssetPath>();
                                        std::string p = ap.GetResolvedPath();
                                        if (p.empty()) p = ap.GetAssetPath();
                                        if (!p.empty()) {
                                            SLOG("SpectralRender: dome shader '%s'.%s = '%s'\n",
                                                    target.GetText(), attr.GetName().GetText(), p.c_str());
                                            if (hdriPath.empty()) hdriPath = p;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Debug: dump all attributes if nothing found
                if (hdriPath.empty()) {
                    SLOG("SpectralRender: dome '%s' — no HDRI found. Attributes:\n",
                            prim.GetPath().GetText());
                    for (const auto& attr : prim.GetAttributes()) {
                        VtValue val;
                        if (attr.Get(&val, timeCode))
                            SLOG("  %s = %s\n", attr.GetName().GetText(), val.GetTypeName().c_str());
                        else
                            SLOG("  %s (no value)\n", attr.GetName().GetText());
                    }
                }

                // Load the HDRI if we found a path
                if (!hdriPath.empty()) {
                    int texId = _scene->LoadTexture(hdriPath);
                    if (texId >= 0) {
                        const auto* tex = _scene->GetTexture(texId);
                        if (tex && tex->IsValid()) {
                            light.envTexId = texId;
                            light.envWidth = tex->GetWidth();
                            light.envHeight = tex->GetHeight();
                            light.envPixels = tex->_pixels.data();
                            light.ComputeEnvAverage();
                            SLOG("SpectralRender: HDRI loaded '%s' (%dx%d) avg=(%.2f,%.2f,%.2f)\n",
                                    hdriPath.c_str(), light.envWidth, light.envHeight,
                                    light.envAvgColor[0], light.envAvgColor[1], light.envAvgColor[2]);
                        }
                    }
                }
            }

            // Common light properties via LightAPI
            {
                VtValue v;
                GfVec3f col(1.f);
                if (lightAPI.GetColorAttr().Get(&v, timeCode) && v.IsHolding<GfVec3f>())
                    col = v.UncheckedGet<GfVec3f>();
                light.color = col;

                float intensity = 1.f;
                if (lightAPI.GetIntensityAttr().Get(&v, timeCode) && v.IsHolding<float>())
                    intensity = v.UncheckedGet<float>();
                light.intensity = intensity;

                float exposure = 0.f;
                if (lightAPI.GetExposureAttr().Get(&v, timeCode) && v.IsHolding<float>())
                    exposure = v.UncheckedGet<float>();
                light.exposure = exposure;

                bool enableTemp = false;
                if (lightAPI.GetEnableColorTemperatureAttr().Get(&v, timeCode) && v.IsHolding<bool>())
                    enableTemp = v.UncheckedGet<bool>();
                light.enableColorTemperature = enableTemp;

                float temp = 6500.f;
                if (lightAPI.GetColorTemperatureAttr().Get(&v, timeCode) && v.IsHolding<float>())
                    temp = v.UncheckedGet<float>();
                light.colorTemperature = temp;
            }

            // Determine illuminant type
            if (light.enableColorTemperature) {
                light.illuminant = SpectralLight::Illuminant::Blackbody;
            } else {
                // Check if colour is close to white → D65, otherwise RGB
                float sum = light.color[0] + light.color[1] + light.color[2];
                float maxC = std::max({light.color[0], light.color[1], light.color[2]});
                float minC = std::min({light.color[0], light.color[1], light.color[2]});
                if (sum > 2.5f && (maxC - minC) < 0.1f)
                    light.illuminant = SpectralLight::Illuminant::D65;
                else
                    light.illuminant = SpectralLight::Illuminant::RGB;
            }

            // Apply global light intensity multiplier
            light.intensity *= _lightIntensity;

            // Apply CIE illuminant override
            if (_illuminant > 0) {
                light.enableColorTemperature = true;
                switch (_illuminant) {
                    case 1: // D50
                        light.illuminant = SpectralLight::Illuminant::D65; // closest match
                        light.colorTemperature = 5003.f;
                        break;
                    case 2: // D65
                        light.illuminant = SpectralLight::Illuminant::D65;
                        light.colorTemperature = 6504.f;
                        break;
                    case 3: // A
                        light.illuminant = SpectralLight::Illuminant::A;
                        light.colorTemperature = 2856.f;
                        break;
                    case 4: // F2
                        light.illuminant = SpectralLight::Illuminant::Blackbody;
                        light.colorTemperature = 4230.f;
                        break;
                    case 5: // F11
                        light.illuminant = SpectralLight::Illuminant::Blackbody;
                        light.colorTemperature = 4000.f;
                        break;
                }
            }

            _scene->AddLight(light);
            SLOG("SpectralRender: light '%s' — type=%d color=(%.2f,%.2f,%.2f) "
                    "intensity=%.2f exposure=%.2f effective=%.2f (multiplier=%.2f)\n",
                    light.name.c_str(), static_cast<int>(light.type),
                    light.color[0], light.color[1], light.color[2],
                    light.intensity, light.exposure, light.EffectiveIntensity(),
                    _lightIntensity);
        }

        // ---- USD PointInstancer (flatten instances at load time) ----
        if (prim.IsA<UsdGeomPointInstancer>()) {
            UsdGeomPointInstancer instancer(prim);
            if (!instancer) continue;

            VtVec3fArray positions;
            instancer.GetPositionsAttr().Get(&positions, timeOpen);
            if (positions.empty()) continue;

            VtIntArray protoIndices;
            instancer.GetProtoIndicesAttr().Get(&protoIndices, timeOpen);

            VtQuathArray orientations;
            instancer.GetOrientationsAttr().Get(&orientations, timeOpen);

            VtVec3fArray scales;
            instancer.GetScalesAttr().Get(&scales, timeOpen);

            SdfPathVector protoTargets;
            instancer.GetPrototypesRel().GetTargets(&protoTargets);

            GfMatrix4d instancerXf = xfCache.GetLocalToWorldTransform(prim);

            SLOG("SpectralRender: PointInstancer '%s' — %zu instances, %zu prototypes\n",
                    prim.GetPath().GetText(), positions.size(), protoTargets.size());

            // For each instance, find its prototype mesh and add transformed triangles
            for (size_t inst = 0; inst < positions.size(); ++inst) {
                int protoIdx = (inst < protoIndices.size()) ? protoIndices[inst] : 0;
                if (protoIdx < 0 || protoIdx >= (int)protoTargets.size()) continue;

                UsdPrim protoPrim = stage->GetPrimAtPath(protoTargets[protoIdx]);
                if (!protoPrim || !protoPrim.IsA<UsdGeomMesh>()) continue;

                UsdGeomMesh protoMesh(protoPrim);
                VtVec3fArray protoPoints;
                protoMesh.GetPointsAttr().Get(&protoPoints, timeOpen);
                if (protoPoints.empty()) continue;

                VtIntArray protoFaceVertexCounts, protoFaceVertexIndices;
                protoMesh.GetFaceVertexCountsAttr().Get(&protoFaceVertexCounts, timeOpen);
                protoMesh.GetFaceVertexIndicesAttr().Get(&protoFaceVertexIndices, timeOpen);

                // Build instance transform
                GfMatrix4d instMat(1.0);
                if (inst < scales.size()) {
                    GfVec3f s = scales[inst];
                    instMat = GfMatrix4d(s[0],0,0,0, 0,s[1],0,0, 0,0,s[2],0, 0,0,0,1);
                }
                if (inst < orientations.size()) {
                    GfQuatd q(orientations[inst]);
                    GfMatrix4d rotMat(1.0); rotMat.SetRotate(GfRotation(q));
                    instMat = instMat * rotMat;
                }
                GfVec3f pos = positions[inst];
                GfMatrix4d transMat(1.0); transMat.SetTranslate(GfVec3d(pos));
                instMat = instMat * transMat * instancerXf;

                // Transform prototype points
                VtVec3fArray xfPoints(protoPoints.size());
                for (size_t v = 0; v < protoPoints.size(); ++v) {
                    GfVec3d wp = instMat.Transform(GfVec3d(protoPoints[v]));
                    xfPoints[v] = GfVec3f(wp);
                }

                // Compute normals
                VtVec3fArray xfNormals(protoPoints.size(), GfVec3f(0.f));
                for (size_t fi = 0; fi + 2 < protoFaceVertexIndices.size(); fi += 3) {
                    int i0 = protoFaceVertexIndices[fi];
                    int i1 = protoFaceVertexIndices[fi+1];
                    int i2 = protoFaceVertexIndices[fi+2];
                    if (i0 >= (int)xfPoints.size() || i1 >= (int)xfPoints.size() || i2 >= (int)xfPoints.size()) continue;
                    GfVec3f fn = GfCross(xfPoints[i1]-xfPoints[i0], xfPoints[i2]-xfPoints[i0]);
                    xfNormals[i0] += fn; xfNormals[i1] += fn; xfNormals[i2] += fn;
                }
                for (auto& n : xfNormals) { float l = n.GetLength(); if (l > 1e-8f) n /= l; }

                // Add triangles (simple fan triangulation for non-tri faces)
                int objectId = _scene->NextObjectId();
                int matId = 0;
                size_t idx = 0;
                SpectralMeshData meshData;
                meshData.id = prim.GetPath().AppendChild(TfToken("inst_" + std::to_string(inst)));
                meshData.objectId = objectId;
                meshData.visible = true;

                for (int fc : protoFaceVertexCounts) {
                    for (int t = 0; t < fc - 2; ++t) {
                        int i0 = protoFaceVertexIndices[idx];
                        int i1 = protoFaceVertexIndices[idx + t + 1];
                        int i2 = protoFaceVertexIndices[idx + t + 2];
                        if (i0 >= (int)xfPoints.size() || i1 >= (int)xfPoints.size() || i2 >= (int)xfPoints.size()) continue;
                        SpectralTriangle tri;
                        tri.v0 = xfPoints[i0]; tri.v1 = xfPoints[i1]; tri.v2 = xfPoints[i2];
                        tri.n0 = xfNormals[i0]; tri.n1 = xfNormals[i1]; tri.n2 = xfNormals[i2];
                        tri.materialId = matId;
                        tri.objectId = objectId;
                        meshData.triangles.push_back(tri);
                    }
                    idx += fc;
                }
                // Auto-smooth normals when scanlineCompat
                if (_scanlineCompat && !meshData.triangles.empty()) {
                    // Position-quantized key — see the main mesh path for rationale.
                    // Quantization tolerance: 1e-5 world units.
                    auto quantize = [](float f) -> int64_t {
                        return static_cast<int64_t>(std::llround(double(f) * 1.0e5));
                    };
                    struct QKey {
                        int64_t x, y, z;
                        bool operator==(const QKey& o) const {
                            return x == o.x && y == o.y && z == o.z;
                        }
                    };
                    struct QKeyHash {
                        size_t operator()(const QKey& k) const {
                            size_t h = 0;
                            h ^= std::hash<int64_t>{}(k.x) + 0x9e3779b9 + (h<<6) + (h>>2);
                            h ^= std::hash<int64_t>{}(k.y) + 0x9e3779b9 + (h<<6) + (h>>2);
                            h ^= std::hash<int64_t>{}(k.z) + 0x9e3779b9 + (h<<6) + (h>>2);
                            return h;
                        }
                    };
                    auto makeKey = [&](const pxr::GfVec3f& v) -> QKey {
                        return { quantize(v[0]), quantize(v[1]), quantize(v[2]) };
                    };
                    std::unordered_map<QKey, pxr::GfVec3f, QKeyHash> vn;
                    for (const auto& tri : meshData.triangles) {
                        vn[makeKey(tri.v0)] += tri.faceNormal;
                        vn[makeKey(tri.v1)] += tri.faceNormal;
                        vn[makeKey(tri.v2)] += tri.faceNormal;
                    }
                    for (auto& kv : vn) { float l = kv.second.GetLength(); if (l>1e-8f) kv.second /= l; }
                    for (auto& tri : meshData.triangles) {
                        if (tri.n0 == tri.n1 && tri.n1 == tri.n2) {
                            auto i0=vn.find(makeKey(tri.v0));
                            auto i1=vn.find(makeKey(tri.v1));
                            auto i2=vn.find(makeKey(tri.v2));
                            if (i0!=vn.end()) tri.n0=i0->second;
                            if (i1!=vn.end()) tri.n1=i1->second;
                            if (i2!=vn.end()) tri.n2=i2->second;
                        }
                    }
                }
                _scene->SetMeshData(meshData.id, std::move(meshData));
            }
            continue;
        }

        // Process meshes
        // First: check for SpectralVDBRead prims (PointsPrim, not Mesh)
        // Extract their world transform for volume positioning
        std::string primName = prim.GetPath().GetString();
        if (primName.find("SpectralVDBRead") != std::string::npos ||
            primName.find("volume_bbox") != std::string::npos) {
            VolumeXform vxf;
            GfMatrix4d xf = xfCache.GetLocalToWorldTransform(prim);
            if (xf != GfMatrix4d(1.0)) {
                vxf.hasXform = true;
                vxf.translate = GfVec3f(float(xf[3][0]), float(xf[3][1]), float(xf[3][2]));
                GfVec3d col0(xf[0][0], xf[0][1], xf[0][2]);
                GfVec3d col1(xf[1][0], xf[1][1], xf[1][2]);
                GfVec3d col2(xf[2][0], xf[2][1], xf[2][2]);
                float sx = float(col0.GetLength());
                float sy = float(col1.GetLength());
                float sz = float(col2.GetLength());
                vxf.scale = GfVec3f(sx, sy, sz);
                if (sx > 1e-6f) { col0 /= sx; }
                if (sy > 1e-6f) { col1 /= sy; }
                if (sz > 1e-6f) { col2 /= sz; }
                float ry = float(std::asin(std::max(-1.0, std::min(1.0, -col2[0]))));
                float rx, rz;
                if (std::abs(col2[0]) < 0.9999) {
                    rx = float(std::atan2(col2[1], col2[2]));
                    rz = float(std::atan2(col1[0], col0[0]));
                } else {
                    rx = float(std::atan2(-col0[2], col1[1]));
                    rz = 0.f;
                }
                vxf.rotate = GfVec3f(rx * 180.f/3.14159265f,
                                     ry * 180.f/3.14159265f,
                                     rz * 180.f/3.14159265f);
                if (!_vdbHasSceneXform) {
                    _vdbHasSceneXform = true;
                    _vdbSceneTranslate = vxf.translate;
                    _vdbSceneRotate = vxf.rotate;
                    _vdbSceneScale = vxf.scale;
                }
                SLOG("SpectralRender: VDB[%d] xform: T=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) S=(%.2f,%.2f,%.2f)\n",
                        (int)_volumeXforms.size(),
                        vxf.translate[0], vxf.translate[1], vxf.translate[2],
                        vxf.rotate[0], vxf.rotate[1], vxf.rotate[2],
                        vxf.scale[0], vxf.scale[1], vxf.scale[2]);
            }
            _volumeXforms.push_back(vxf);
            continue;
        }

        // Skip non-mesh prims (lights, cameras, etc.)
        if (!prim.IsA<UsdGeomMesh>()) continue;

        UsdGeomMesh mesh(prim);
        if (!mesh) continue;

        VtVec3fArray points;
        mesh.GetPointsAttr().Get(&points, timeOpen);
        if (points.empty()) continue;
        VtVec3fArray pointsRef;  // undisplaced positions for pRef AOV

        // Per-mesh property overrides from SpectralMeshProperties.
        // These are populated once at mesh scope (outside any material
        // binding), then applied in the subdiv block below AND to the
        // material after it's been resolved by either the USD or the
        // Nuke-native code path.
        int     meshSubdivOverride       = 0;
        int     meshSchemeOverride       = -1;
        bool    meshFlipNormals          = false;
        int     meshNormalMode           = 0;    // 0=auto 1=smooth 2=faceted 3=vertex
        bool    meshPropsUseDisplayColor = false;
        GfVec3f meshPropsDisplayColor    (0.8f, 0.8f, 0.8f);
        float   meshPropsDisplayOpacity  = 1.0f;
        bool    meshPropsVisible         = true;
        bool    meshPropsCastsShadows    = true;
        bool    meshPropsHasEntry        = false;

        // TODO: replace substring / size==1 matching with prim-path
        // registration done inside SpectralMeshProperties::processScenegraph.
        // Current matching is brittle: with >1 modifier node nothing matches.
        {
            std::string meshPath = prim.GetPath().GetString();
            const auto& meshReg = SpectralMeshPropertiesOp::GetRegistry();
            for (const auto& entry : meshReg) {
                if (meshPath.find(entry.first) != std::string::npos
                    || meshReg.size() == 1) {
                    meshSubdivOverride = entry.second.subdivLevel;
                    if (entry.second.subdivScheme > 0)
                        meshSchemeOverride = entry.second.subdivScheme;
                    meshFlipNormals          = entry.second.flipNormals;
                    meshNormalMode           = entry.second.normalMode;
                    meshPropsUseDisplayColor = entry.second.useDisplayColor;
                    meshPropsDisplayColor    = GfVec3f(
                        entry.second.displayColor[0],
                        entry.second.displayColor[1],
                        entry.second.displayColor[2]);
                    meshPropsDisplayOpacity  = entry.second.displayOpacity;
                    meshPropsVisible         = entry.second.visible;
                    meshPropsCastsShadows    = entry.second.castsShadows;
                    meshPropsHasEntry        = true;
                    SLOG("SpectralRender: mesh props for '%s' via node '%s' "
                         "(level=%d scheme=%d flipN=%d normMode=%d)\n",
                         meshPath.c_str(), entry.first.c_str(),
                         meshSubdivOverride, meshSchemeOverride,
                         (int)meshFlipNormals, meshNormalMode);
                    break;
                }
            }
            if (!meshPropsHasEntry && !meshReg.empty()) {
                SLOG("SpectralRender: mesh '%s' - no matching "
                     "SpectralMeshProperties entry (registry size=%zu)\n",
                     meshPath.c_str(), meshReg.size());
            }
        }

        // Read points at shutter close for motion blur.
        //
        // Two paths here, because the open/close stages may or may not be
        // distinct objects:
        //   - USD file input (stageClose null): the stage has time samples,
        //     so reading mesh.GetPointsAttr() at timeClose returns the
        //     deformed-at-close values, and UsdGeomXformCache(timeClose)
        //     resolves the xform at close.
        //   - Nuke 3D node graph input (stageClose non-null): each stage is
        //     a static snapshot built by BuildStage at one time value.
        //     Time-parametric reads on a single stage return identical
        //     results, so we have to look up the prim in stageClose and
        //     read its default-time values.
        VtVec3fArray pointsClose;
        bool meshHasMotion = false;
        GfMatrix4d chainWorldOpen(1.0);   // identity unless nodeGraphInput
        GfMatrix4d chainWorldClose(1.0);
        bool useChainWalk = false;
        UsdPrim closePrim;  // only used when stageClose is non-null
        if (motionBlurEnabled) {
            // Node-graph input: chain-walk matrices trump anything the USG
            // bridge gives us for xform motion, because the bridge bakes
            // at current frame and its time-parametric reads return
            // identical values regardless of sampleTime.
            if (nodeGraphInput) {
                const double tOpen  = static_cast<double>(_frame) + _shutterOpen;
                const double tClose = static_cast<double>(_frame) + _shutterClose;
                chainWorldOpen  = _ComputeInputChainWorldMatrix(nodeGraphInput, tOpen);
                chainWorldClose = _ComputeInputChainWorldMatrix(nodeGraphInput, tClose);
                useChainWalk = true;
            }

            if (stageClose) {
                closePrim = stageClose->GetPrimAtPath(prim.GetPath());
                if (closePrim && closePrim.IsA<UsdGeomMesh>()) {
                    UsdGeomMesh closeMesh(closePrim);
                    closeMesh.GetPointsAttr().Get(&pointsClose, UsdTimeCode::Default());
                } else {
                    mesh.GetPointsAttr().Get(&pointsClose, timeClose);
                }
            } else {
                mesh.GetPointsAttr().Get(&pointsClose, timeClose);
            }

            // Check if local positions differ (vertex deformation)
            bool pointsDiffer = false;
            if (pointsClose.size() == points.size()) {
                for (size_t pi = 0; pi < points.size(); ++pi) {
                    if ((points[pi] - pointsClose[pi]).GetLengthSq() > 1e-10f) {
                        pointsDiffer = true;
                        break;
                    }
                }
            }
            if (pointsDiffer) meshHasMotion = true;

            // Check if the world transform differs (rigid body motion).
            // This is the branch that catches GeoTransform, animated Axis,
            // and other Nuke 3D ops that don't deform vertices but move
            // the whole mesh through space.
            bool xfDiffers = false;
            if (!meshHasMotion) {
                if (useChainWalk) {
                    // Chain-walk path: compare matrices we computed above.
                    // This is the one that actually catches node-graph
                    // motion; USG-based xfCache doesn't.
                    if (chainWorldOpen != chainWorldClose) {
                        xfDiffers = true;
                        meshHasMotion = true;
                        pointsClose = points;  // no deformation, just xform
                    }
                } else {
                    // USD file input: read xform via cache at the two times.
                    // Works because the file has native time samples.
                    UsdGeomXformCache xfCacheOpen(timeOpen);
                    UsdGeomXformCache xfCacheClose(timeClose);
                    GfMatrix4d xfOpen  = xfCacheOpen.GetLocalToWorldTransform(prim);
                    GfMatrix4d xfClose = xfCacheClose.GetLocalToWorldTransform(prim);
                    if (xfOpen != xfClose) {
                        xfDiffers = true;
                        meshHasMotion = true;
                        pointsClose = points;
                    }
                }
            }

            // Diagnostic -- if the user reports "no motion blur on my
            // animated mesh" this log tells us whether the detector saw
            // the motion or not. Without it we're guessing.
            SLOG("SpectralRender: mblur detect '%s': path=%s "
                 "pointsDiffer=%d xfDiffers=%d -> hasMotion=%d\n",
                 prim.GetPath().GetText(),
                 useChainWalk ? "chain-walk" : "usd-file",
                 (int)pointsDiffer, (int)xfDiffers, (int)meshHasMotion);

            if (!meshHasMotion) pointsClose.clear();
        }

        VtIntArray faceVertexCounts;
        VtIntArray faceVertexIndices;
        mesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts, timeCode);
        mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, timeCode);
        if (faceVertexCounts.empty() || faceVertexIndices.empty()) continue;

        VtVec3fArray normals;
        mesh.GetNormalsAttr().Get(&normals, timeCode);
        TfToken normalsInterp = mesh.GetNormalsInterpolation();

        // ----------------------------------------------------------
        // UV coordinates (Phase 5: texture mapping)
        //   Read "st" or "uv" primvar for texture coordinates.
        // ----------------------------------------------------------
        VtVec2fArray uvs;
        TfToken uvsInterp;
        {
            UsdGeomPrimvarsAPI pvAPI(prim);
            UsdGeomPrimvar stPrimvar = pvAPI.GetPrimvar(TfToken("st"));
            if (!stPrimvar) stPrimvar = pvAPI.GetPrimvar(TfToken("uv"));
            if (stPrimvar) {
                stPrimvar.Get(&uvs, timeCode);
                uvsInterp = stPrimvar.GetInterpolation();
            }
        }

        // ----------------------------------------------------------
        // Material reading (Phase 4)
        //   Read UsdPreviewSurface material bound to this mesh.
        //   Extract baseColor, metallic, roughness, etc.
        // ----------------------------------------------------------
        SpectralMaterialId matId = kDefaultMaterialId;
        {
            UsdShadeMaterialBindingAPI bindingAPI(prim);
            UsdShadeMaterial boundMat = bindingAPI.ComputeBoundMaterial();
            if (boundMat) {
                UsdShadeShader surfaceShader = boundMat.ComputeSurfaceSource();
                if (surfaceShader) {
                    SpectralMaterial mat;
                    mat.name = boundMat.GetPath().GetString();

                    // Debug: print shader ID and all inputs
                    TfToken surfShaderId;
                    surfaceShader.GetIdAttr().Get(&surfShaderId, timeCode);
                    SLOG("SpectralRender: shader id='%s' prim='%s'\n",
                            surfShaderId.GetText(), surfaceShader.GetPath().GetText());

                    // List all inputs
                    auto inputs = surfaceShader.GetInputs();
                    for (const auto& inp : inputs) {
                        VtValue val;
                        inp.Get(&val, timeCode);

                        // Check for connections
                        UsdShadeConnectableAPI connSrc;
                        TfToken connName;
                        UsdShadeAttributeType connType;
                        bool connected = UsdShadeConnectableAPI::GetConnectedSource(
                            inp, &connSrc, &connName, &connType);

                        if (connected) {
                            UsdShadeShader connShader(connSrc.GetPrim());
                            TfToken connId;
                            if (connShader) connShader.GetIdAttr().Get(&connId, timeCode);
                            SLOG("  input '%s' -> connected to '%s' (id='%s')\n",
                                    inp.GetBaseName().GetText(),
                                    connSrc.GetPrim().GetPath().GetText(),
                                    connId.GetText());
                        } else if (val.IsHolding<SdfAssetPath>()) {
                            SLOG("  input '%s' = asset:'%s'\n",
                                    inp.GetBaseName().GetText(),
                                    val.UncheckedGet<SdfAssetPath>().GetAssetPath().c_str());
                        }
                    }

                    // Read UsdPreviewSurface inputs (with type safety)
                    auto readFloat = [&](const char* inputName, float& val) {
                        try {
                            UsdShadeInput inp = surfaceShader.GetInput(TfToken(inputName));
                            if (!inp) return;
                            VtValue v;
                            inp.Get(&v, timeCode);
                            if (v.IsHolding<float>()) val = v.UncheckedGet<float>();
                            else if (v.IsHolding<double>()) val = float(v.UncheckedGet<double>());
                            else if (v.IsHolding<int>()) val = float(v.UncheckedGet<int>());
                        } catch (...) {}
                    };
                    auto readColor = [&](const char* inputName, GfVec3f& val) {
                        try {
                            UsdShadeInput inp = surfaceShader.GetInput(TfToken(inputName));
                            if (!inp) return;
                            VtValue v;
                            inp.Get(&v, timeCode);
                            if (v.IsHolding<GfVec3f>()) val = v.UncheckedGet<GfVec3f>();
                            else if (v.IsHolding<GfVec4f>()) {
                                GfVec4f v4 = v.UncheckedGet<GfVec4f>();
                                val = GfVec3f(v4[0], v4[1], v4[2]);
                            }
                            else if (v.IsHolding<float>()) {
                                float f = v.UncheckedGet<float>();
                                val = GfVec3f(f, f, f);
                            }
                        } catch (...) {}
                    };

                    readColor("diffuseColor", mat.baseColor);
                    readFloat("metallic", mat.metallic);
                    readFloat("roughness", mat.roughness);
                    readFloat("ior", mat.ior);
                    readFloat("opacity", mat.opacity);
                    readColor("emissiveColor", mat.emissiveColor);
                    readFloat("clearcoat", mat.clearcoat);
                    readFloat("clearcoatRoughness", mat.clearcoatRoughness);

                    // ── MaterialX Standard Surface ──
                    // MtlXStandardSurface uses different input names
                    {
                        GfVec3f mtlxBaseColor(0.f);
                        readColor("base_color", mtlxBaseColor);
                        if (mtlxBaseColor != GfVec3f(0.f)) {
                            float baseWeight = 1.f;
                            readFloat("base", baseWeight);
                            mat.baseColor = mtlxBaseColor * baseWeight;
                        }
                        float metalness = -1.f;
                        readFloat("metalness", metalness);
                        if (metalness >= 0.f) mat.metallic = metalness;

                        float specRough = -1.f;
                        readFloat("specular_roughness", specRough);
                        if (specRough >= 0.f) mat.roughness = specRough;

                        float specIOR = -1.f;
                        readFloat("specular_IOR", specIOR);
                        if (specIOR > 0.f) mat.ior = specIOR;

                        // Specular weight scales Fresnel response
                        // Modulate IOR: specular=0 → IOR=1 (no reflection), specular=1 → original IOR
                        float specWeight = 1.f;
                        readFloat("specular", specWeight);
                        if (specWeight < 0.99f && mat.ior > 1.0f) {
                            mat.ior = 1.f + (mat.ior - 1.f) * specWeight;
                        }

                        // Specular color tints the reflection for dielectrics
                        GfVec3f specColor(1.f);
                        readColor("specular_color", specColor);
                        // Apply specular tint: for metals it tints reflection, for dielectrics
                        // it modulates F0. We approximate by blending base color toward specular
                        // tint at high specular weight for metallic materials.
                        if (mat.metallic > 0.5f && specColor != GfVec3f(1.f)) {
                            GfVec3f tint(0.5f + specColor[0]*0.5f, 0.5f + specColor[1]*0.5f, 0.5f + specColor[2]*0.5f);
                            mat.baseColor = GfVec3f(mat.baseColor[0]*tint[0], mat.baseColor[1]*tint[1], mat.baseColor[2]*tint[2]);
                        }

                        float coat = -1.f;
                        readFloat("coat", coat);
                        if (coat >= 0.f) mat.clearcoat = coat;

                        float coatRough = -1.f;
                        readFloat("coat_roughness", coatRough);
                        if (coatRough >= 0.f) mat.clearcoatRoughness = coatRough;

                        float emission = -1.f;
                        readFloat("emission", emission);
                        if (emission > 0.f) {
                            GfVec3f emCol(1.f);
                            readColor("emission_color", emCol);
                            mat.emissiveColor = emCol * emission;
                        }

                        // MtlX opacity can be color3 or float
                        {
                            GfVec3f opacCol(1.f);
                            readColor("opacity", opacCol);
                            float opacAvg = (opacCol[0] + opacCol[1] + opacCol[2]) / 3.f;
                            if (opacAvg < 0.99f) mat.opacity = opacAvg;
                        }

                        float transmission = 0.f;
                        readFloat("transmission", transmission);
                        if (transmission > 0.01f) {
                            mat.opacity = std::max(0.f, mat.opacity * (1.f - transmission));
                            GfVec3f transCol(1.f);
                            readColor("transmission_color", transCol);
                            mat.absorptionColor = GfVec3f(1.f) - transCol;
                        }
                    }

                    // ── Preview Surface ──
                    // Nuke's PreviewSurface — match ScanlineRender brightness
                    if (surfShaderId == TfToken("PreviewSurface") ||
                        surfShaderId == TfToken("NukePreviewSurface") ||
                        surfShaderId == TfToken("UsdPreviewSurface")) {
                        // ScanlineRender uses a simpler lighting model that produces
                        // dimmer results than physically-based spectral rendering.
                        // Scale diffuse down to match ScanlineRender output.
                        mat.baseColor = GfVec3f(mat.baseColor[0]*0.6f, mat.baseColor[1]*0.6f, mat.baseColor[2]*0.6f);
                    }

                    // ── ReflectiveSurface ──
                    // Nuke's reflective surface shader
                    {
                        float reflectivity = -1.f;
                        readFloat("reflectivity", reflectivity);
                        if (reflectivity >= 0.f) {
                            mat.metallic = reflectivity;
                            mat.roughness = std::max(0.05f, mat.roughness);
                        }
                        GfVec3f reflColor(0.f);
                        readColor("reflection_color", reflColor);
                        if (reflColor != GfVec3f(0.f)) {
                            // Reflective surface uses reflection color as base for metallic look
                            mat.baseColor = reflColor;
                            if (mat.metallic < 0.5f) mat.metallic = 0.8f;
                        }
                    }

                    // ── WireframeShader ──
                    if (surfShaderId == TfToken("WireframeShader") ||
                        surfShaderId == TfToken("NukeWireframeShader")) {
                        mat.opacity = 0.01f;  // near-transparent surface for wireframe-only
                        _wireframeEnable = true;  // auto-enable wireframe overlay
                        SLOG("SpectralRender: WireframeShader detected — enabling wireframe overlay\n");
                    }

                    // ── Shadow Catcher ──
                    if (surfShaderId == TfToken("ShadowCatcher") ||
                        surfShaderId == TfToken("NukeShadowCatcher") ||
                        surfShaderId == TfToken("shadow_catcher")) {
                        _shadowCatcherMatIds.insert(-1);  // temp key, remapped after AddMaterial
                        mat.isShadowCatcher = true;
                        mat.baseColor = GfVec3f(1.f);
                        mat.roughness = 1.f;
                        mat.metallic = 0.f;
                        SLOG("SpectralRender: shadow catcher material detected\n");
                    }
                    // Also detect shadow catcher from material name
                    {
                        std::string lowerName = mat.name;
                        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                        if (lowerName.find("shadowcatch") != std::string::npos ||
                            lowerName.find("shadow_catch") != std::string::npos) {
                            _shadowCatcherMatIds.insert(-1);
                            mat.isShadowCatcher = true;
                            mat.baseColor = GfVec3f(1.f);
                            mat.roughness = 1.f;
                            mat.metallic = 0.f;
                            SLOG("SpectralRender: shadow catcher from name '%s'\n", mat.name.c_str());
                        }
                        // Check user-supplied shadow catcher material names
                        if (_shadowCatcherNames && _shadowCatcherNames[0] != '\0') {
                            std::string nameList(_shadowCatcherNames);
                            std::string lowerList(nameList);
                            std::transform(lowerList.begin(), lowerList.end(), lowerList.begin(), ::tolower);
                            // Split by comma
                            size_t pos = 0;
                            while (pos < lowerList.size()) {
                                size_t end = lowerList.find(',', pos);
                                if (end == std::string::npos) end = lowerList.size();
                                std::string token = lowerList.substr(pos, end - pos);
                                // Trim whitespace
                                size_t s = token.find_first_not_of(" \t");
                                size_t e = token.find_last_not_of(" \t");
                                if (s != std::string::npos && e != std::string::npos) {
                                    token = token.substr(s, e - s + 1);
                                    if (!token.empty() && lowerName.find(token) != std::string::npos) {
                                        _shadowCatcherMatIds.insert(-1);
                                        mat.isShadowCatcher = true;
                                        mat.baseColor = GfVec3f(1.f);
                                        mat.roughness = 1.f;
                                        mat.metallic = 0.f;
                                        SLOG("SpectralRender: shadow catcher from user name '%s' matched '%s'\n",
                                                token.c_str(), mat.name.c_str());
                                    }
                                }
                                pos = end + 1;
                            }
                        }
                    }

                    // ── Project3D ──
                    // Camera projection mapping: project texture through a camera
                    if (surfShaderId == TfToken("NukeProject3D") ||
                        surfShaderId == TfToken("Project3D") ||
                        surfShaderId == TfToken("project3d")) {
                        SLOG("SpectralRender: detected Project3D shader\n");

                        // Try to read projection matrix from shader inputs
                        GfMatrix4d projVP(1.0);
                        bool gotProjection = false;

                        // Method 1: direct matrix inputs
                        auto readMatrix = [&](const char* name) -> GfMatrix4d {
                            UsdShadeInput inp = surfaceShader.GetInput(TfToken(name));
                            if (inp) {
                                VtValue v; inp.Get(&v, timeCode);
                                if (v.IsHolding<GfMatrix4d>()) return v.UncheckedGet<GfMatrix4d>();
                            }
                            return GfMatrix4d(1.0);
                        };

                        // Try cam_projection * cam_view
                        {
                            UsdShadeInput camInp = surfaceShader.GetInput(TfToken("cam"));
                            if (!camInp) camInp = surfaceShader.GetInput(TfToken("camera"));
                            if (camInp) {
                                UsdShadeConnectableAPI camSrc;
                                TfToken camName;
                                UsdShadeAttributeType camType;
                                if (UsdShadeConnectableAPI::GetConnectedSource(
                                        camInp, &camSrc, &camName, &camType)) {
                                    // Connected to a camera prim — get its matrices
                                    UsdPrim camPrim = camSrc.GetPrim();
                                    if (camPrim) {
                                        UsdGeomCamera geomCam(camPrim);
                                        if (geomCam) {
                                            GfCamera gfCam = geomCam.GetCamera(timeCode);
                                            GfMatrix4d view = gfCam.GetTransform().GetInverse();
                                            GfFrustum frust = gfCam.GetFrustum();
                                            GfMatrix4d proj = frust.ComputeProjectionMatrix();
                                            projVP = view * proj;
                                            gotProjection = true;
                                            SLOG("SpectralRender: Project3D camera from USD prim '%s'\n",
                                                    camPrim.GetPath().GetText());
                                        }
                                    }
                                }
                            }
                        }

                        // Method 2: explicit matrix inputs on the shader
                        if (!gotProjection) {
                            GfMatrix4d mView = readMatrix("worldToCamera");
                            GfMatrix4d mProj = readMatrix("projection");
                            if (mView != GfMatrix4d(1.0) || mProj != GfMatrix4d(1.0)) {
                                projVP = mView * mProj;
                                gotProjection = true;
                            }
                        }

                        // Method 3: try cam_world_to_ndc combined matrix
                        if (!gotProjection) {
                            GfMatrix4d mVP = readMatrix("cam_world_to_ndc");
                            if (mVP != GfMatrix4d(1.0)) {
                                projVP = mVP;
                                gotProjection = true;
                            }
                        }

                        if (gotProjection) {
                            // Store the VP matrix — UVs will be recomputed after mesh loading
                            // matId hasn't been assigned yet, so use _projCameraVP with a temp key
                            // We'll remap after AddMaterial
                            _projCameraVP[-1] = projVP;  // temp key, remapped below
                        }
                    }

                    // Fallback property names (BasicMaterial, Phong, generic)
                    if (mat.baseColor == GfVec3f(0.18f, 0.18f, 0.18f)) {
                        // Still at default — try alternative names
                        readColor("diffuse_color", mat.baseColor);
                        readColor("color", mat.baseColor);
                        readColor("baseColor", mat.baseColor);
                    }
                    // Shininess → roughness conversion
                    {
                        float shininess = -1.f;
                        readFloat("shininess", shininess);
                        if (shininess >= 0.f && mat.roughness >= 0.49f) {
                            mat.roughness = std::max(0.05f, 1.f - std::sqrt(std::min(shininess / 100.f, 1.f)));
                        }
                    }
                    // Specular → metallic estimate
                    {
                        GfVec3f specCol(0.f);
                        readColor("specularColor", specCol);
                        if (specCol == GfVec3f(0.f)) readColor("specular_color", specCol);
                        float specLum = (specCol[0] + specCol[1] + specCol[2]) / 3.f;
                        if (specLum > 0.01f && mat.metallic < 0.01f) {
                            mat.metallic = std::min(specLum * 2.f, 1.f);
                        }
                    }
                    // Transparency → opacity
                    {
                        float transp = -1.f;
                        readFloat("transparency", transp);
                        if (transp >= 0.f) mat.opacity = 1.f - transp;
                    }

                    // Determine texture input names based on shader type
                    const char* diffuseTexInput = "diffuseColor";
                    const char* roughTexInput   = "roughness";
                    const char* metalTexInput   = "metallic";

                    if (surfShaderId == TfToken("NukeDefaultSurface") ||
                        surfShaderId == TfToken("NukeBasicMaterial") ||
                        surfShaderId == TfToken("NukePhong") ||
                        surfShaderId == TfToken("PreviewSurface") ||
                        surfShaderId == TfToken("NukePreviewSurface") ||
                        surfShaderId == TfToken("ReflectiveSurface") ||
                        surfShaderId == TfToken("NukeReflectiveSurface")) {
                        // Nuke's shaders use different input names
                        diffuseTexInput = "tex_color";
                        roughTexInput = nullptr;
                        metalTexInput = nullptr;
                    }

                    // MaterialX Standard Surface texture inputs
                    if (surfShaderId == TfToken("ND_standard_surface_surfaceshader") ||
                        surfShaderId == TfToken("MtlXStandardSurface") ||
                        surfShaderId == TfToken("standard_surface")) {
                        diffuseTexInput = "base_color";
                        roughTexInput = "specular_roughness";
                        metalTexInput = "metalness";
                    }

                    // Check for texture connections (UsdUVTexture or Nuke Iop textures)
                    auto readTexture = [&](const char* inputName) -> int {
                        UsdShadeInput inp = surfaceShader.GetInput(TfToken(inputName));
                        if (!inp) return -1;

                        // Check if connected to another shader
                        UsdShadeConnectableAPI src;
                        TfToken srcName;
                        UsdShadeAttributeType srcType;
                        if (!UsdShadeConnectableAPI::GetConnectedSource(
                                inp, &src, &srcName, &srcType))
                            return -1;

                        UsdShadeShader texShader(src.GetPrim());
                        if (!texShader) return -1;

                        TfToken shaderId;
                        texShader.GetIdAttr().Get(&shaderId, timeCode);

                        // --- Path 1: UsdUVTexture ---
                        if (shaderId == TfToken("UsdUVTexture")) {
                            UsdShadeInput fileInp = texShader.GetInput(TfToken("file"));
                            if (!fileInp) return -1;

                            VtValue fileVal;
                            fileInp.Get(&fileVal, timeCode);
                            std::string filePath;
                            std::string assetPath;
                            if (fileVal.IsHolding<SdfAssetPath>()) {
                                SdfAssetPath ap = fileVal.UncheckedGet<SdfAssetPath>();
                                filePath = ap.GetResolvedPath();
                                assetPath = ap.GetAssetPath();
                                if (filePath.empty()) filePath = assetPath;
                            } else if (fileVal.IsHolding<std::string>()) {
                                filePath = fileVal.UncheckedGet<std::string>();
                                assetPath = filePath;
                            }

                            if (filePath.empty()) return -1;

                            // Check if it's a Nuke Iop reference (not a real file)
                            if (assetPath.find("/NkRoot/") != std::string::npos) {
                                Op* texOp = ShaderOp::retrieveOpFromAssetPath(assetPath);
                                Iop* texIop = dynamic_cast<Iop*>(texOp);
                                if (texIop) {
                                    try {
                                        texIop->validate(true);
                                        const int texW = texIop->info().format().width();
                                        const int texH = texIop->info().format().height();
                                        if (texW > 0 && texH > 0) {
                                            pxr::SpectralTexture tex;
                                            tex._width = texW;
                                            tex._height = texH;
                                            tex._channels = 4;
                                            tex._pixels.resize(size_t(texW) * texH * 4);
                                            tex._path = assetPath;

                                            texIop->request(0, 0, texW, texH, Mask_RGBA, 1);
                                            for (int y = 0; y < texH; ++y) {
                                                Row row(0, texW);
                                                texIop->get(y, 0, texW, Mask_RGBA, row);
                                                const float* rp = row[Chan_Red];
                                                const float* gp = row[Chan_Green];
                                                const float* bp = row[Chan_Blue];
                                                const float* ap = row[Chan_Alpha];
                                                int storeY = texH - 1 - y;
                                                for (int x = 0; x < texW; ++x) {
                                                    size_t idx = (size_t(storeY) * texW + x) * 4;
                                                    tex._pixels[idx + 0] = rp ? rp[x] : 0.f;
                                                    tex._pixels[idx + 1] = gp ? gp[x] : 0.f;
                                                    tex._pixels[idx + 2] = bp ? bp[x] : 0.f;
                                                    tex._pixels[idx + 3] = ap ? ap[x] : 1.f;
                                                }
                                            }

                                            int texId = _scene->AddTexture(std::move(tex));
                                            SLOG("SpectralRender: Iop texture '%s' -> %s (%dx%d, id=%d)\n",
                                                    inputName, texIop->node_name(), texW, texH, texId);
                                            return texId;
                                        }
                                    } catch (...) {
                                        SLOG("SpectralRender: failed to render Iop texture '%s'\n",
                                                inputName);
                                    }
                                }
                                return -1;
                            }

                            // Real file on disk
                            int texId = _scene->LoadTexture(filePath);
                            if (texId >= 0) {
                                SLOG("SpectralRender: texture '%s' -> '%s' (id=%d)\n",
                                        inputName, filePath.c_str(), texId);
                            }
                            return texId;
                        }

                        // --- Path 2: Nuke Iop texture (CheckerBoard, etc) ---
                        // Look for an asset path referencing a Nuke Op
                        UsdShadeInput fileInp = texShader.GetInput(TfToken("file"));
                        if (fileInp) {
                            VtValue fileVal;
                            fileInp.Get(&fileVal, timeCode);
                            std::string assetPath;
                            if (fileVal.IsHolding<SdfAssetPath>()) {
                                SdfAssetPath ap = fileVal.UncheckedGet<SdfAssetPath>();
                                assetPath = ap.GetAssetPath();
                            } else if (fileVal.IsHolding<std::string>()) {
                                assetPath = fileVal.UncheckedGet<std::string>();
                            }

                            if (!assetPath.empty() && assetPath.find("/NkRoot/") != std::string::npos) {
                                // It's a Nuke Iop reference — render it to a texture buffer
                                Op* texOp = ShaderOp::retrieveOpFromAssetPath(assetPath);
                                Iop* texIop = dynamic_cast<Iop*>(texOp);
                                if (texIop) {
                                    try {
                                        texIop->validate(true);
                                        const int texW = texIop->info().format().width();
                                        const int texH = texIop->info().format().height();
                                        if (texW > 0 && texH > 0) {
                                            // Render the Iop into a pixel buffer
                                            pxr::SpectralTexture tex;
                                            tex._width = texW;
                                            tex._height = texH;
                                            tex._channels = 4;
                                            tex._pixels.resize(size_t(texW) * texH * 4);
                                            tex._path = assetPath;

                                            texIop->request(0, 0, texW, texH, Mask_RGBA, 1);
                                            for (int y = 0; y < texH; ++y) {
                                                Row row(0, texW);
                                                texIop->get(y, 0, texW, Mask_RGBA, row);
                                                const float* rp = row[Chan_Red];
                                                const float* gp = row[Chan_Green];
                                                const float* bp = row[Chan_Blue];
                                                const float* ap = row[Chan_Alpha];
                                                int storeY = texH - 1 - y;
                                                for (int x = 0; x < texW; ++x) {
                                                    size_t idx = (size_t(storeY) * texW + x) * 4;
                                                    tex._pixels[idx + 0] = rp ? rp[x] : 0.f;
                                                    tex._pixels[idx + 1] = gp ? gp[x] : 0.f;
                                                    tex._pixels[idx + 2] = bp ? bp[x] : 0.f;
                                                    tex._pixels[idx + 3] = ap ? ap[x] : 1.f;
                                                }
                                            }

                                            int texId = _scene->AddTexture(std::move(tex));
                                            SLOG("SpectralRender: Iop texture '%s' -> %s (%dx%d, id=%d)\n",
                                                    inputName, texIop->node_name(), texW, texH, texId);
                                            return texId;
                                        }
                                    } catch (...) {
                                        SLOG("SpectralRender: failed to render Iop texture '%s'\n",
                                                inputName);
                                    }
                                }
                            }
                        }

                        // --- Path 3: Project3DTexture ---
                        // Nuke's Project3D shader wraps the texture in a Project3DTexture node.
                        // Walk its inputs to find the underlying image Iop AND projection camera.
                        if (shaderId == TfToken("Project3DTexture")) {
                            int foundTexId = -1;

                            // Log all inputs for debugging
                            auto proj3dInputs = texShader.GetInputs();
                            for (const auto& p3inp : proj3dInputs) {
                                SLOG("  Project3DTexture input '%s'\n", p3inp.GetBaseName().GetText());
                            }

                            // Read camera matrices directly from Project3DTexture inputs
                            {
                                auto readMat4 = [&](const char* name) -> GfMatrix4d {
                                    UsdShadeInput inp = texShader.GetInput(TfToken(name));
                                    if (!inp) { SLOG("  %s: not found\n", name); return GfMatrix4d(1.0); }

                                    // Try direct value first
                                    VtValue v; inp.Get(&v, timeCode);
                                    if (v.IsHolding<GfMatrix4d>()) return v.UncheckedGet<GfMatrix4d>();
                                    if (v.IsHolding<GfMatrix4f>()) {
                                        GfMatrix4f mf = v.UncheckedGet<GfMatrix4f>();
                                        GfMatrix4d md;
                                        for (int r=0;r<4;r++) for (int c=0;c<4;c++) md[r][c]=mf[r][c];
                                        return md;
                                    }

                                    // Follow connection if direct value is empty
                                    UsdShadeConnectableAPI src;
                                    TfToken srcName;
                                    UsdShadeAttributeType srcType;
                                    if (UsdShadeConnectableAPI::GetConnectedSource(inp, &src, &srcName, &srcType)) {
                                        UsdPrim srcPrim = src.GetPrim();
                                        SLOG("  %s: connected to '%s' output '%s'\n", name,
                                                srcPrim.GetPath().GetText(), srcName.GetText());

                                        // Try the specific named output first
                                        std::string outName = "outputs:" + srcName.GetString();
                                        UsdAttribute outAttr = srcPrim.GetAttribute(TfToken(outName));
                                        if (outAttr) {
                                            VtValue ov; outAttr.Get(&ov, timeCode);
                                            SLOG("  %s: '%s' type='%s'\n", name, outName.c_str(), ov.GetTypeName().c_str());
                                            if (ov.IsHolding<GfMatrix4d>()) return ov.UncheckedGet<GfMatrix4d>();
                                        }

                                        // Try inputs: prefixed version of the output name
                                        std::string inpName = "inputs:" + srcName.GetString();
                                        UsdAttribute inpAttr = srcPrim.GetAttribute(TfToken(inpName));
                                        if (inpAttr) {
                                            VtValue iv; inpAttr.Get(&iv, timeCode);
                                            SLOG("  %s: '%s' type='%s'\n", name, inpName.c_str(), iv.GetTypeName().c_str());
                                            if (iv.IsHolding<GfMatrix4d>()) return iv.UncheckedGet<GfMatrix4d>();
                                        }

                                        // Try bare attribute name
                                        UsdAttribute bareAttr = srcPrim.GetAttribute(srcName);
                                        if (bareAttr) {
                                            VtValue bv; bareAttr.Get(&bv, timeCode);
                                            if (bv.IsHolding<GfMatrix4d>()) return bv.UncheckedGet<GfMatrix4d>();
                                        }

                                        // Last resort: log all matrix attributes on source prim
                                        for (const auto& attr : srcPrim.GetAttributes()) {
                                            VtValue av; attr.Get(&av, timeCode);
                                            if (av.IsHolding<GfMatrix4d>()) {
                                                SLOG("  %s: prim has matrix attr '%s'\n", name,
                                                        attr.GetName().GetText());
                                            }
                                        }
                                    }

                                    // Try reading from the underlying USD attribute directly
                                    UsdAttribute rawAttr = texShader.GetPrim().GetAttribute(
                                        TfToken("inputs:" + std::string(name)));
                                    if (rawAttr) {
                                        VtValue rv; rawAttr.Get(&rv, timeCode);
                                        SLOG("  %s raw attr type='%s' empty=%d\n", name,
                                                rv.GetTypeName().c_str(), rv.IsEmpty());
                                        if (rv.IsHolding<GfMatrix4d>()) return rv.UncheckedGet<GfMatrix4d>();
                                    }

                                    SLOG("  %s: could not resolve (type='%s' empty=%d)\n", name,
                                            v.GetTypeName().c_str(), v.IsEmpty());
                                    return GfMatrix4d(1.0);
                                };

                                GfMatrix4d camProj = readMat4("cam_projection");
                                GfMatrix4d camXform = readMat4("cam_xform");
                                GfMatrix4d fmtXform = readMat4("format_xform");

                                SLOG("  cam_projection[0][0]=%.4f [1][1]=%.4f\n", camProj[0][0], camProj[1][1]);
                                SLOG("  cam_xform[3][0]=%.4f [3][1]=%.4f [3][2]=%.4f\n", camXform[3][0], camXform[3][1], camXform[3][2]);
                                SLOG("  format_xform[0][0]=%.4f [1][1]=%.4f\n", fmtXform[0][0], fmtXform[1][1]);

                                bool hasCamData = (camProj != GfMatrix4d(1.0) || camXform != GfMatrix4d(1.0));
                                if (hasCamData) {
                                    GfMatrix4d camView = camXform.GetInverse();
                                    _projCameraVP[-1] = camView * camProj * fmtXform;
                                    SLOG("SpectralRender: Project3D camera from USD matrices (cam_projection + cam_xform)\n");
                                } else {
                                    SLOG("SpectralRender: Project3D — cam matrices are identity, projection not applied\n");
                                }
                            }

                            // Search for image Iop in inputs
                            for (const auto& p3inp : proj3dInputs) {
                                VtValue p3val;
                                p3inp.Get(&p3val, timeCode);
                                std::string p3path;
                                if (p3val.IsHolding<SdfAssetPath>()) {
                                    p3path = p3val.UncheckedGet<SdfAssetPath>().GetAssetPath();
                                } else if (p3val.IsHolding<std::string>()) {
                                    p3path = p3val.UncheckedGet<std::string>();
                                }

                                // Check for Nuke Iop reference (image or camera)
                                if (!p3path.empty() && p3path.find("/NkRoot/") != std::string::npos) {
                                    Op* p3op = ShaderOp::retrieveOpFromAssetPath(p3path);

                                    // Check if it's a camera
                                    CameraOp* p3cam = dynamic_cast<CameraOp*>(p3op);
                                    if (p3cam) {
                                        try {
                                            p3cam->validate(true);
                                            // Build VP matrix from camera
                                            const Matrix4& camWorld = p3cam->matrix();
                                            Matrix4 camView = camWorld.inverse();

                                            // Build projection matrix from camera knobs
                                            double fov = 45.0;
                                            Knob* fovK = p3cam->knob("focal");
                                            Knob* hapK = p3cam->knob("haperture");
                                            if (fovK && hapK) {
                                                double focal = fovK->get_value();
                                                double hap = hapK->get_value();
                                                if (focal > 0 && hap > 0)
                                                    fov = 2.0 * std::atan(hap / (2.0 * focal)) * 180.0 / M_PI;
                                            }
                                            double aspect = 1.3162;
                                            double zNear = 0.1, zFar = 10000.0;
                                            double f = 1.0 / std::tan(fov * M_PI / 360.0);

                                            GfMatrix4d proj(0.0);
                                            proj[0][0] = f / aspect;
                                            proj[1][1] = f;
                                            proj[2][2] = -(zFar + zNear) / (zFar - zNear);
                                            proj[2][3] = -1.0;
                                            proj[3][2] = -2.0 * zFar * zNear / (zFar - zNear);

                                            GfMatrix4d view(1.0);
                                            for (int r = 0; r < 4; ++r)
                                                for (int c = 0; c < 4; ++c)
                                                    view[r][c] = camView[c][r]; // transpose for row-major

                                            _projCameraVP[-1] = view * proj;
                                            SLOG("SpectralRender: Project3D camera '%s' (fov=%.1f)\n",
                                                    p3cam->node_name(), fov);
                                        } catch (...) {
                                            SLOG("SpectralRender: failed to read Project3D camera\n");
                                        }
                                        continue;
                                    }

                                    // Check if it's an image Iop
                                    Iop* p3iop = dynamic_cast<Iop*>(p3op);
                                    if (p3iop && foundTexId < 0) {
                                        try {
                                            p3iop->validate(true);
                                            const int tw = p3iop->info().format().width();
                                            const int th = p3iop->info().format().height();
                                            if (tw > 0 && th > 0) {
                                                pxr::SpectralTexture tex;
                                                tex._width = tw; tex._height = th;
                                                tex._channels = 4;
                                                tex._pixels.resize(size_t(tw) * th * 4);
                                                tex._path = p3path;
                                                p3iop->request(0, 0, tw, th, Mask_RGBA, 1);
                                                for (int y = 0; y < th; ++y) {
                                                    Row row(0, tw);
                                                    p3iop->get(y, 0, tw, Mask_RGBA, row);
                                                    const float* rp = row[Chan_Red];
                                                    const float* gp = row[Chan_Green];
                                                    const float* bp = row[Chan_Blue];
                                                    const float* ap = row[Chan_Alpha];
                                                    int storeY = th - 1 - y;
                                                    for (int x = 0; x < tw; ++x) {
                                                        size_t idx = (size_t(storeY) * tw + x) * 4;
                                                        tex._pixels[idx+0] = rp ? rp[x] : 0.f;
                                                        tex._pixels[idx+1] = gp ? gp[x] : 0.f;
                                                        tex._pixels[idx+2] = bp ? bp[x] : 0.f;
                                                        tex._pixels[idx+3] = ap ? ap[x] : 1.f;
                                                    }
                                                }
                                                foundTexId = _scene->AddTexture(std::move(tex));
                                                SLOG("SpectralRender: Project3D texture '%s' -> %s (%dx%d, id=%d)\n",
                                                        inputName, p3iop->node_name(), tw, th, foundTexId);
                                            }
                                        } catch (...) {
                                            SLOG("SpectralRender: failed to read Project3D texture\n");
                                        }
                                    }
                                }

                                // Check connected shaders for file inputs
                                UsdShadeConnectableAPI p3src;
                                TfToken p3srcName;
                                UsdShadeAttributeType p3srcType;
                                if (foundTexId < 0 && UsdShadeConnectableAPI::GetConnectedSource(
                                        p3inp, &p3src, &p3srcName, &p3srcType)) {
                                    UsdShadeShader connShader(p3src.GetPrim());
                                    if (connShader) {
                                        UsdShadeInput connFile = connShader.GetInput(TfToken("file"));
                                        if (connFile) {
                                            VtValue cfv; connFile.Get(&cfv, timeCode);
                                            std::string cfp;
                                            if (cfv.IsHolding<SdfAssetPath>())
                                                cfp = cfv.UncheckedGet<SdfAssetPath>().GetAssetPath();
                                            if (!cfp.empty() && cfp.find("/NkRoot/") != std::string::npos) {
                                                Op* cfOp = ShaderOp::retrieveOpFromAssetPath(cfp);
                                                Iop* cfIop = dynamic_cast<Iop*>(cfOp);
                                                if (cfIop) {
                                                    try {
                                                        cfIop->validate(true);
                                                        const int tw = cfIop->info().format().width();
                                                        const int th = cfIop->info().format().height();
                                                        if (tw > 0 && th > 0) {
                                                            pxr::SpectralTexture tex;
                                                            tex._width = tw; tex._height = th;
                                                            tex._channels = 4;
                                                            tex._pixels.resize(size_t(tw) * th * 4);
                                                            tex._path = cfp;
                                                            cfIop->request(0, 0, tw, th, Mask_RGBA, 1);
                                                            for (int y = 0; y < th; ++y) {
                                                                Row row(0, tw);
                                                                cfIop->get(y, 0, tw, Mask_RGBA, row);
                                                                const float* rp = row[Chan_Red];
                                                                const float* gp = row[Chan_Green];
                                                                const float* bp = row[Chan_Blue];
                                                                const float* ap = row[Chan_Alpha];
                                                                int storeY = th - 1 - y;
                                                                for (int x = 0; x < tw; ++x) {
                                                                    size_t idx = (size_t(storeY)*tw+x)*4;
                                                                    tex._pixels[idx+0] = rp ? rp[x] : 0.f;
                                                                    tex._pixels[idx+1] = gp ? gp[x] : 0.f;
                                                                    tex._pixels[idx+2] = bp ? bp[x] : 0.f;
                                                                    tex._pixels[idx+3] = ap ? ap[x] : 1.f;
                                                                }
                                                            }
                                                            foundTexId = _scene->AddTexture(std::move(tex));
                                                            SLOG("SpectralRender: Project3D connected texture '%s' -> %s (%dx%d, id=%d)\n",
                                                                    inputName, cfIop->node_name(), tw, th, foundTexId);
                                                        }
                                                    } catch (...) {}
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            if (foundTexId >= 0) return foundTexId;
                            SLOG("SpectralRender: Project3DTexture — could not find image Iop in inputs\n");
                        }

                        SLOG("SpectralRender: unsupported texture shader '%s' on '%s'\n",
                                shaderId.GetText(), inputName);
                        return -1;
                    };

                    mat.baseColorTexId = readTexture(diffuseTexInput);
                    mat.roughnessTexId = roughTexInput ? readTexture(roughTexInput) : -1;
                    mat.metallicTexId  = metalTexInput ? readTexture(metalTexInput) : -1;

                    // Check for spectral properties from SpectralSurface nodes
                    {
                        std::string shaderPath = surfaceShader.GetPath().GetString();
                        // Try each registered SpectralSurface node name
                        const auto& registry = SpectralSurfaceOp::GetRegistry();
                        for (const auto& entry : registry) {
                            if (shaderPath.find(entry.first) != std::string::npos) {
                                mat.abbeNumber        = entry.second.abbeNumber;
                                mat.thinFilmThickness = entry.second.thinFilmThickness;
                                mat.displacementScale = entry.second.displacementScale;
                                mat.displacementMidpoint = entry.second.displacementMidpoint;
                                mat.displacementMode = entry.second.dispType;
                                mat.metalType = entry.second.metalType;
                                mat.textureBlend = entry.second.textureBlend;
                                mat.absorptionColor = GfVec3f(entry.second.absorptionColor[0],
                                                               entry.second.absorptionColor[1],
                                                               entry.second.absorptionColor[2]);
                                mat.absorptionDensity = entry.second.absorptionDensity;
                                mat.gratingSpacing = entry.second.gratingSpacing;
                                mat.gratingStrength = entry.second.gratingStrength;
                                mat.fluorAbsorb = entry.second.fluorAbsorb;
                                mat.fluorEmit = entry.second.fluorEmit;
                                mat.fluorStrength = entry.second.fluorStrength;
                                mat.sssColor = GfVec3f(entry.second.sssColor[0],
                                                        entry.second.sssColor[1],
                                                        entry.second.sssColor[2]);
                                mat.sssRadius = entry.second.sssRadius;
                                // Load displacement texture from file path
                                if (!entry.second.displacementFile.empty()
                                    && mat.displacementScale > 0.f) {
                                    int dTexId = _scene->LoadTexture(entry.second.displacementFile);
                                    if (dTexId >= 0) {
                                        mat.displacementTexId = dTexId;
                                        SLOG("SpectralRender: displacement map '%s' (id=%d)\n",
                                                entry.second.displacementFile.c_str(), dTexId);
                                    }
                                }
                                // Fall back to displacement Iop from SpectralSurface input
                                if (mat.displacementTexId < 0 && mat.displacementScale > 0.f
                                    && entry.second.dispIop) {
                                    Iop* dispIop = dynamic_cast<Iop*>(entry.second.dispIop);
                                    if (dispIop) {
                                        try {
                                            dispIop->validate(true);
                                            const int texW = dispIop->info().format().width();
                                            const int texH = dispIop->info().format().height();
                                            if (texW > 0 && texH > 0) {
                                                pxr::SpectralTexture tex;
                                                tex._width = texW;
                                                tex._height = texH;
                                                tex._channels = 3;
                                                tex._pixels.resize(size_t(texW) * texH * 3);
                                                tex._path = "displacement_iop";

                                                dispIop->request(0, 0, texW, texH, Mask_RGB, 1);
                                                for (int y = 0; y < texH; ++y) {
                                                    Row row(0, texW);
                                                    dispIop->get(y, 0, texW, Mask_RGB, row);
                                                    const float* rp = row[Chan_Red];
                                                    const float* gp = row[Chan_Green];
                                                    const float* bp = row[Chan_Blue];
                                                    int storeY = texH - 1 - y;
                                                    for (int x = 0; x < texW; ++x) {
                                                        size_t idx = (size_t(storeY) * texW + x) * 3;
                                                        tex._pixels[idx + 0] = rp ? rp[x] : 0.f;
                                                        tex._pixels[idx + 1] = gp ? gp[x] : 0.f;
                                                        tex._pixels[idx + 2] = bp ? bp[x] : 0.f;
                                                    }
                                                }

                                                mat.displacementTexId = _scene->AddTexture(std::move(tex));
                                                SLOG("SpectralRender: displacement from Iop '%s' (%dx%d, id=%d)\n",
                                                        dispIop->node_name(), texW, texH, mat.displacementTexId);
                                            }
                                        } catch (...) {
                                            SLOG("SpectralRender: failed to read displacement Iop\n");
                                        }
                                    }
                                }

                                // Read base color texture from tex Iop pipe
                                if (mat.baseColorTexId < 0 && entry.second.texIop) {
                                    Iop* texIop = dynamic_cast<Iop*>(entry.second.texIop);
                                    if (texIop) {
                                        try {
                                            texIop->validate(true);
                                            const int texW = texIop->info().format().width();
                                            const int texH = texIop->info().format().height();
                                            if (texW > 0 && texH > 0) {
                                                pxr::SpectralTexture tex;
                                                tex._width = texW;
                                                tex._height = texH;
                                                tex._channels = 4;
                                                tex._pixels.resize(size_t(texW) * texH * 4);
                                                tex._path = "tex_iop";
                                                texIop->request(0, 0, texW, texH, Mask_RGBA, 1);
                                                for (int y = 0; y < texH; ++y) {
                                                    Row row(0, texW);
                                                    texIop->get(y, 0, texW, Mask_RGBA, row);
                                                    const float* rp = row[Chan_Red];
                                                    const float* gp = row[Chan_Green];
                                                    const float* bp = row[Chan_Blue];
                                                    const float* ap = row[Chan_Alpha];
                                                    int storeY = texH - 1 - y;
                                                    for (int x = 0; x < texW; ++x) {
                                                        size_t idx = (size_t(storeY) * texW + x) * 4;
                                                        tex._pixels[idx+0] = rp ? rp[x] : 0.f;
                                                        tex._pixels[idx+1] = gp ? gp[x] : 0.f;
                                                        tex._pixels[idx+2] = bp ? bp[x] : 0.f;
                                                        tex._pixels[idx+3] = ap ? ap[x] : 1.f;
                                                    }
                                                }
                                                mat.baseColorTexId = _scene->AddTexture(std::move(tex));
                                                SLOG("SpectralRender: base color from tex pipe '%s' (%dx%d, id=%d)\n",
                                                        texIop->node_name(), texW, texH, mat.baseColorTexId);
                                            }
                                        } catch (...) {
                                            SLOG("SpectralRender: failed to read texture Iop\n");
                                        }
                                    }
                                }

                                // Read bump map from bump Iop pipe
                                if (mat.bumpMapTexId < 0 && entry.second.mapIop && entry.second.mapMode == 0) {
                                    Iop* bumpIop = dynamic_cast<Iop*>(entry.second.mapIop);
                                    if (bumpIop) {
                                        try {
                                            bumpIop->validate(true);
                                            const int bW = bumpIop->info().format().width();
                                            const int bH = bumpIop->info().format().height();
                                            if (bW > 0 && bH > 0) {
                                                pxr::SpectralTexture tex;
                                                tex._width = bW;
                                                tex._height = bH;
                                                tex._channels = 3;
                                                tex._pixels.resize(size_t(bW) * bH * 3);
                                                tex._path = "bump_iop";
                                                bumpIop->request(0, 0, bW, bH, Mask_RGB, 1);
                                                for (int y = 0; y < bH; ++y) {
                                                    Row row(0, bW);
                                                    bumpIop->get(y, 0, bW, Mask_RGB, row);
                                                    const float* rp = row[Chan_Red];
                                                    const float* gp = row[Chan_Green];
                                                    const float* bp = row[Chan_Blue];
                                                    int storeY = bH - 1 - y;
                                                    for (int x = 0; x < bW; ++x) {
                                                        size_t idx = (size_t(storeY) * bW + x) * 3;
                                                        tex._pixels[idx+0] = rp ? rp[x] : 0.f;
                                                        tex._pixels[idx+1] = gp ? gp[x] : 0.f;
                                                        tex._pixels[idx+2] = bp ? bp[x] : 0.f;
                                                    }
                                                }
                                                mat.bumpMapTexId = _scene->AddTexture(std::move(tex));
                                                mat.bumpStrength = entry.second.bumpStrength;
                                                SLOG("SpectralRender: bump map from pipe '%s' (%dx%d, id=%d, strength=%.2f)\n",
                                                        bumpIop->node_name(), bW, bH, mat.bumpMapTexId, mat.bumpStrength);
                                            }
                                        } catch (...) {
                                            SLOG("SpectralRender: failed to read bump Iop\n");
                                        }
                                    }
                                }

                                if (mat.abbeNumber > 0.f || mat.thinFilmThickness > 0.f
                                    || mat.displacementScale > 0.f) {
                                    SLOG("SpectralRender: spectral props from '%s'"
                                            " — Abbe=%.1f thinFilm=%.0fnm disp=%.2f\n",
                                            entry.first.c_str(),
                                            mat.abbeNumber, mat.thinFilmThickness,
                                            mat.displacementScale);
                                }
                                break;
                            }
                        }
                    }

                    // Check for SpectralDrafting node assigned to this material
                    {
                        std::string shaderPath = surfaceShader.GetPath().GetString();
                        const auto& wireReg = SpectralDraftingOp::GetRegistry();
                        for (const auto& entry : wireReg) {
                            if (shaderPath.find(entry.first) != std::string::npos) {
                                _wireframeEnable = true;
                                _wireThickness = entry.second.thickness;
                                _wireOpacity = entry.second.opacity;
                                _wireColor[0] = entry.second.color[0];
                                _wireColor[1] = entry.second.color[1];
                                _wireColor[2] = entry.second.color[2];
                                _wireDashed = entry.second.dashed;
                                _wireDashLength = entry.second.dashLength;
                                _wireGapLength = entry.second.gapLength;
                                _wireNth = std::max(1, int(entry.second.gridDensity));
                                _wireStyle = entry.second.style;
                                // Architectural
                                _archSilhouetteWeight = entry.second.archSilhouetteWeight;
                                _archMediumWeight = entry.second.archMediumWeight;
                                _archThinWeight = entry.second.archThinWeight;
                                _archSilhouetteColor[0] = entry.second.archSilhouetteColor[0];
                                _archSilhouetteColor[1] = entry.second.archSilhouetteColor[1];
                                _archSilhouetteColor[2] = entry.second.archSilhouetteColor[2];
                                _archThinOpacity = entry.second.archThinOpacity;
                                // Pencil
                                _pencilWobble = entry.second.pencilWobble;
                                _pencilPressure = entry.second.pencilPressure;
                                _pencilCrossHatch = entry.second.pencilCrossHatch;
                                _pencilHatchDensity = entry.second.pencilHatchDensity;
                                _pencilHatchAngle = entry.second.pencilHatchAngle;
                                // Topo
                                _topoDirection = entry.second.topoDirection;
                                _topoUpVector[0] = entry.second.topoUpVector[0];
                                _topoUpVector[1] = entry.second.topoUpVector[1];
                                _topoUpVector[2] = entry.second.topoUpVector[2];
                                _topoContourInterval = entry.second.topoContourInterval;
                                _topoMajorEvery = entry.second.topoMajorEvery;
                                _wireAAMode = entry.second.aaMode;
                                _wireAAWidth = entry.second.aaWidth;
                                mat.opacity = 0.02f;
                                SLOG("SpectralRender: wireframe from SpectralDrafting '%s'\n",
                                        entry.first.c_str());
                                break;
                            }
                        }
                    }

                    // Check for SpectralShadowCatcher node assigned to this material
                    {
                        std::string shaderPath = surfaceShader.GetPath().GetString();
                        const auto& scReg = SpectralShadowCatcherOp::GetRegistry();
                        for (const auto& entry : scReg) {
                            if (shaderPath.find(entry.first) != std::string::npos) {
                                _shadowCatcherMatIds.insert(-1);
                                mat.isShadowCatcher = true;
                                mat.baseColor = GfVec3f(1.f);
                                mat.roughness = 1.f;
                                mat.metallic = 0.f;
                                SLOG("SpectralRender: shadow catcher from SpectralShadowCatcher '%s'\n",
                                        entry.first.c_str());
                                break;
                            }
                        }
                    }

                    // -- Apply mesh-level overrides (collected at mesh scope) --
                    // The lookup itself was moved out to mesh scope so it
                    // runs regardless of whether material binding succeeds.
                    // Here we just apply the display/visibility overrides to
                    // the USD-resolved material before it gets committed.
                    if (meshPropsHasEntry) {
                        if (meshPropsUseDisplayColor) {
                            mat.baseColor = meshPropsDisplayColor;
                        }
                        mat.opacity *= meshPropsDisplayOpacity;
                        if (!meshPropsVisible) mat.opacity = 0.f;
                    }

                    matId = _scene->AddMaterial(mat);

                    // Remap Project3D projection matrix from temp key to actual matId
                    if (_projCameraVP.count(-1)) {
                        _projCameraVP[matId] = _projCameraVP[-1];
                        _projCameraVP.erase(-1);
                        SLOG("SpectralRender: Project3D projection assigned to material %d\n", matId);
                    }

                    // Remap shadow catcher from temp key to actual matId
                    if (_shadowCatcherMatIds.count(-1)) {
                        _shadowCatcherMatIds.erase(-1);
                        _shadowCatcherMatIds.insert(matId);
                        SLOG("SpectralRender: shadow catcher assigned to material %d\n", matId);
                    }

                    // Remap no-shadow-cast from temp key to actual matId.
                    // Temp-key insert done just before AddMaterial; see the
                    // block that tags meshPropsCastsShadows earlier in this function.
                    if (_noShadowCastMatIds.count(-1)) {
                        _noShadowCastMatIds.erase(-1);
                        _noShadowCastMatIds.insert(matId);
                        SLOG("SpectralRender: castsShadows=false for material %d\n", matId);
                    }

                    SLOG("SpectralRender: material '%s' — color=(%.2f,%.2f,%.2f) metal=%.2f rough=%.2f opacity=%.2f\n",
                            mat.name.c_str(),
                            mat.baseColor[0], mat.baseColor[1], mat.baseColor[2],
                            mat.metallic, mat.roughness, mat.opacity);
                }
            }
        }

        // ----------------------------------------------------------
        //   Fallback: read Nuke native material from Op chain
        //   Handles BasicMaterial, Phong, FlatShading connected
        //   to the scene input when USD material binding fails.
        // ----------------------------------------------------------
        if (matId == kDefaultMaterialId) {
            // Walk the scene input chain to find material Ops
            Op* scnIn = (inputs() > 1) ? input(1) : nullptr;
            if (scnIn) {
                // Walk up to 8 levels of input chain looking for material knobs
                Op* cur = scnIn;
                for (int depth = 0; depth < 8 && cur; ++depth) {
                    // Check if this Op has material-like knobs
                    Knob* kDiffColor = cur->knob("diffuse_color");
                    Knob* kColor     = cur->knob("color");
                    Knob* kShininess = cur->knob("shininess");
                    Knob* kSpecColor = cur->knob("specular_color");
                    Knob* kOpacity   = cur->knob("opacity");
                    Knob* kEmission  = cur->knob("emission");
                    Knob* kTransparency = cur->knob("transparency");

                    if (kDiffColor || kColor) {
                        SpectralMaterial mat;
                        mat.name = std::string("nuke_native_") + cur->node_name();

                        // Diffuse color
                        if (kDiffColor) {
                            mat.baseColor = pxr::GfVec3f(
                                float(kDiffColor->get_value(0)),
                                float(kDiffColor->get_value(1)),
                                float(kDiffColor->get_value(2)));
                        } else if (kColor) {
                            mat.baseColor = pxr::GfVec3f(
                                float(kColor->get_value(0)),
                                float(kColor->get_value(1)),
                                float(kColor->get_value(2)));
                        }

                        // Shininess → roughness (inverse mapping)
                        if (kShininess) {
                            float shininess = float(kShininess->get_value());
                            // Map shininess 0-100 → roughness 1.0-0.05
                            mat.roughness = std::max(0.05f, 1.f - std::sqrt(std::min(shininess / 100.f, 1.f)));
                        }

                        // Specular color → derive metallic
                        if (kSpecColor) {
                            float sr = float(kSpecColor->get_value(0));
                            float sg = float(kSpecColor->get_value(1));
                            float sb = float(kSpecColor->get_value(2));
                            float specLum = (sr + sg + sb) / 3.f;
                            // High specular luminance = more metallic
                            mat.metallic = std::min(specLum * 2.f, 1.f);
                        }

                        // Opacity / transparency
                        if (kOpacity) {
                            mat.opacity = float(kOpacity->get_value());
                        } else if (kTransparency) {
                            mat.opacity = 1.f - float(kTransparency->get_value());
                        }

                        // Emission
                        if (kEmission) {
                            float em = float(kEmission->get_value());
                            if (em > 0.01f) {
                                mat.emissiveColor = mat.baseColor * em;
                            }
                        }

                        // Check for texture input (input 0 on material nodes is usually the texture)
                        if (cur->inputs() > 0 && cur->input(0)) {
                            Iop* texIop = dynamic_cast<Iop*>(cur->input(0));
                            if (texIop) {
                                try {
                                    texIop->validate(true);
                                    const int texW = texIop->info().format().width();
                                    const int texH = texIop->info().format().height();
                                    if (texW > 0 && texH > 0 && texW <= 8192 && texH <= 8192) {
                                        pxr::SpectralTexture tex;
                                        tex._width = texW;
                                        tex._height = texH;
                                        tex._channels = 4;
                                        tex._pixels.resize(size_t(texW) * texH * 4);
                                        tex._path = std::string("nuke_tex_") + texIop->node_name();
                                        texIop->request(0, 0, texW, texH, Mask_RGBA, 1);
                                        for (int y = 0; y < texH; ++y) {
                                            Row row(0, texW);
                                            texIop->get(y, 0, texW, Mask_RGBA, row);
                                            const float* rp = row[Chan_Red];
                                            const float* gp = row[Chan_Green];
                                            const float* bp = row[Chan_Blue];
                                            const float* ap = row[Chan_Alpha];
                                            int storeY = texH - 1 - y;
                                            for (int x = 0; x < texW; ++x) {
                                                size_t idx = (size_t(storeY) * texW + x) * 4;
                                                tex._pixels[idx+0] = rp ? rp[x] : 0.f;
                                                tex._pixels[idx+1] = gp ? gp[x] : 0.f;
                                                tex._pixels[idx+2] = bp ? bp[x] : 0.f;
                                                tex._pixels[idx+3] = ap ? ap[x] : 1.f;
                                            }
                                        }
                                        mat.baseColorTexId = _scene->AddTexture(std::move(tex));
                                        mat.textureBlend = 1.f;
                                        SLOG("SpectralRender: native texture '%s' (%dx%d, id=%d)\n",
                                                texIop->node_name(), texW, texH, mat.baseColorTexId);
                                    }
                                } catch (...) {
                                    SLOG("SpectralRender: failed to read native material texture\n");
                                }
                            }
                        }

                        // Apply mesh-level overrides (same logic as USD path).
                        if (meshPropsHasEntry) {
                            if (meshPropsUseDisplayColor) {
                                mat.baseColor = meshPropsDisplayColor;
                            }
                            mat.opacity *= meshPropsDisplayOpacity;
                            if (!meshPropsVisible) mat.opacity = 0.f;
                        }

                        matId = _scene->AddMaterial(mat);
                        SLOG("SpectralRender: native material '%s' from '%s' — "
                             "color=(%.2f,%.2f,%.2f) metal=%.2f rough=%.2f opacity=%.2f\n",
                             mat.name.c_str(), cur->node_name(),
                             mat.baseColor[0], mat.baseColor[1], mat.baseColor[2],
                             mat.metallic, mat.roughness, mat.opacity);
                        break;  // found material, stop walking
                    }

                    // Walk to first input that's not a camera
                    cur = (cur->inputs() > 0) ? cur->input(0) : nullptr;
                }
            }
        }

        // ----------------------------------------------------------
        // Subdivision (Phase 2)
        // ----------------------------------------------------------
#ifdef SPECTRAL_HAS_OSD
        {
            TfToken subdivScheme;
            mesh.GetSubdivisionSchemeAttr().Get(&subdivScheme, timeCode);
            std::string schemeStr = subdivScheme.GetString();

            SpectralSubdiv::Scheme scheme = SpectralSubdiv::SchemeFromToken(schemeStr);

            // Mesh properties scheme override
            if (meshSchemeOverride >= 0) {
                static const SpectralSubdiv::Scheme overrideSchemes[] = {
                    SpectralSubdiv::Scheme::None,       // 0 = auto (unused, meshSchemeOverride > 0 here)
                    SpectralSubdiv::Scheme::CatmullClark,
                    SpectralSubdiv::Scheme::Loop,
                    SpectralSubdiv::Scheme::Bilinear,
                    SpectralSubdiv::Scheme::None         // 4 = none
                };
                if (meshSchemeOverride < 5)
                    scheme = overrideSchemes[meshSchemeOverride];
            }

            // If the user set a level override via SpectralMeshProperties
            // but scheme is still None (because neither the source mesh's
            // subdivisionScheme nor the user's scheme knob specified one),
            // force CatmullClark so the level override actually takes effect.
            // Without this, setting level=4 with scheme=auto silently does
            // nothing when the source mesh has no subdivisionScheme.
            if (meshSubdivOverride > 0
                && scheme == SpectralSubdiv::Scheme::None
                && meshSchemeOverride < 0) {
                scheme = SpectralSubdiv::Scheme::CatmullClark;
                SLOG("SpectralRender: mesh '%s' - level override %d forces "
                     "CatmullClark (source mesh had no subdivisionScheme)\n",
                        prim.GetPath().GetText(), meshSubdivOverride);
            }

            // Auto-subdivide when displacement is present but no scheme set
            const SpectralMaterial& meshMat = _scene->GetMaterial(matId);
            if (scheme == SpectralSubdiv::Scheme::None
                && meshMat.displacementScale > 0.f
                && meshMat.displacementTexId >= 0) {
                scheme = SpectralSubdiv::Scheme::CatmullClark;
                SLOG("SpectralRender: auto-subdividing %s for displacement\n",
                        prim.GetPath().GetText());
            }

            if (scheme != SpectralSubdiv::Scheme::None) {
                SpectralSubdiv::Input subIn;
                subIn.points            = points;
                subIn.faceVertexCounts  = faceVertexCounts;
                subIn.faceVertexIndices = faceVertexIndices;
                subIn.scheme            = scheme;
                subIn.level             = (meshSubdivOverride > 0) ? meshSubdivOverride : 2;

                // Read crease edges from USD
                {
                    VtIntArray creaseIndices, creaseLengths;
                    VtFloatArray creaseSharpnesses;
                    mesh.GetCreaseIndicesAttr().Get(&creaseIndices, timeOpen);
                    mesh.GetCreaseLengthsAttr().Get(&creaseLengths, timeOpen);
                    mesh.GetCreaseSharpnessesAttr().Get(&creaseSharpnesses, timeOpen);

                    if (!creaseLengths.empty() && !creaseSharpnesses.empty()) {
                        // Expand crease chains into edge pairs for OpenSubdiv
                        VtIntArray edgePairs;
                        VtFloatArray edgeSharpnesses;
                        size_t idx = 0;
                        for (size_t c = 0; c < creaseLengths.size(); ++c) {
                            int len = creaseLengths[c];
                            float sharpness = (c < creaseSharpnesses.size())
                                ? creaseSharpnesses[c] : creaseSharpnesses.back();
                            for (int e = 0; e < len - 1 && idx + 1 < creaseIndices.size(); ++e) {
                                edgePairs.push_back(creaseIndices[idx]);
                                edgePairs.push_back(creaseIndices[idx + 1]);
                                edgeSharpnesses.push_back(sharpness);
                                ++idx;
                            }
                            if (len > 0) ++idx;  // advance past chain end
                        }
                        subIn.creaseIndices = edgePairs;
                        subIn.creaseSharpnesses = edgeSharpnesses;

                        if (!edgePairs.empty())
                            SLOG("SpectralRender: %zu crease edges (sharpness %.1f-%.1f)\n",
                                    edgeSharpnesses.size(),
                                    *std::min_element(edgeSharpnesses.begin(), edgeSharpnesses.end()),
                                    *std::max_element(edgeSharpnesses.begin(), edgeSharpnesses.end()));
                    }

                    // Read corner vertices
                    VtIntArray cornerIndices;
                    VtFloatArray cornerSharpnesses;
                    mesh.GetCornerIndicesAttr().Get(&cornerIndices, timeOpen);
                    mesh.GetCornerSharpnessesAttr().Get(&cornerSharpnesses, timeOpen);
                    if (!cornerIndices.empty() && !cornerSharpnesses.empty()) {
                        subIn.cornerIndices = cornerIndices;
                        subIn.cornerSharpnesses = cornerSharpnesses;
                        SLOG("SpectralRender: %zu corner vertices\n", cornerIndices.size());
                    }
                }

                if (normalsInterp == UsdGeomTokens->vertex &&
                    static_cast<int>(normals.size()) == static_cast<int>(points.size())) {
                    subIn.normals = normals;
                }

                // Pass UVs to subdivider
                if (!uvs.empty()) {
                    if (uvsInterp == UsdGeomTokens->faceVarying) {
                        // Face-varying: each face-vertex has its own UV
                        // UV indices are identity (0, 1, 2, ...)
                        subIn.uvs = uvs;
                        subIn.uvIndices.resize(uvs.size());
                        for (int i = 0; i < static_cast<int>(uvs.size()); ++i)
                            subIn.uvIndices[i] = i;
                        subIn.uvIsFaceVarying = true;
                    } else if (uvsInterp == UsdGeomTokens->vertex) {
                        // Vertex: UVs indexed same as positions
                        subIn.uvs = uvs;
                        subIn.uvIndices = faceVertexIndices;
                        subIn.uvIsFaceVarying = true;
                    }
                }

                SpectralSubdiv::Output subOut;
                if (SpectralSubdiv::Refine(subIn, subOut)) {
                    points = subOut.points;

                    if (!subOut.normals.empty()) {
                        normals = subOut.normals;
                        normalsInterp = UsdGeomTokens->vertex;
                    } else {
                        // Compute smooth normals from refined mesh
                        const int nPts = static_cast<int>(points.size());
                        const int nIdx = static_cast<int>(subOut.triangleIndices.size());
                        normals.resize(nPts);
                        for (int i = 0; i < nPts; ++i)
                            normals[i] = GfVec3f(0.f);

                        for (int i = 0; i + 2 < nIdx; i += 3) {
                            int i0 = subOut.triangleIndices[i];
                            int i1 = subOut.triangleIndices[i + 1];
                            int i2 = subOut.triangleIndices[i + 2];
                            if (i0 >= nPts || i1 >= nPts || i2 >= nPts) continue;
                            GfVec3f e0 = points[i1] - points[i0];
                            GfVec3f e1 = points[i2] - points[i0];
                            GfVec3f fn = GfCross(e0, e1);
                            normals[i0] += fn;
                            normals[i1] += fn;
                            normals[i2] += fn;
                        }
                        for (int i = 0; i < nPts; ++i) {
                            float len = normals[i].GetLength();
                            normals[i] = (len > 1e-8f)
                                ? normals[i] / len
                                : GfVec3f(0.f, 1.f, 0.f);
                        }
                        normalsInterp = UsdGeomTokens->vertex;
                    }

                    const int numTris = static_cast<int>(subOut.triangleIndices.size()) / 3;
                    faceVertexCounts.resize(numTris);
                    for (int i = 0; i < numTris; ++i) faceVertexCounts[i] = 3;
                    faceVertexIndices.resize(subOut.triangleIndices.size());
                    for (size_t i = 0; i < subOut.triangleIndices.size(); ++i)
                        faceVertexIndices[i] = subOut.triangleIndices[i];

                    // Map the Scheme enum back to a readable name for logging.
                    const char* appliedScheme = "unknown";
                    switch (scheme) {
                        case SpectralSubdiv::Scheme::CatmullClark: appliedScheme = "catmullClark"; break;
                        case SpectralSubdiv::Scheme::Loop:         appliedScheme = "loop";         break;
                        case SpectralSubdiv::Scheme::Bilinear:     appliedScheme = "bilinear";     break;
                        case SpectralSubdiv::Scheme::None:         appliedScheme = "none";         break;
                    }
                    SLOG("SpectralRender: mesh %s subdivided (%s level %d)\n",
                            prim.GetPath().GetText(), appliedScheme, subIn.level);

                    // Use refined UVs from subdivision
                    if (!subOut.uvs.empty() && !subOut.uvIndices.empty()) {
                        // Flatten face-varying UVs: create one UV per face-vertex
                        // so the face-varying lookup in triangulation works directly
                        size_t numFaceVerts = subOut.uvIndices.size();
                        uvs.resize(numFaceVerts);
                        for (size_t i = 0; i < numFaceVerts; ++i) {
                            int uvIdx = subOut.uvIndices[i];
                            if (uvIdx >= 0 && uvIdx < static_cast<int>(subOut.uvs.size())) {
                                uvs[i] = subOut.uvs[uvIdx];
                            } else {
                                uvs[i] = GfVec2f(0.f);
                            }
                        }
                        uvsInterp = UsdGeomTokens->faceVarying;
                        SLOG("SpectralRender: refined %zu UVs for %s\n",
                                uvs.size(), prim.GetPath().GetText());
                    } else {
                        uvs.clear();
                    }

                    // ---- Render-time displacement ----
                    // Save undisplaced positions for pRef AOV
                    VtVec3fArray pointsRef = points;

                    // After subdivision: offset refined vertices along normals
                    // using displacement map sampled at refined UVs.
                    {
                        const SpectralMaterial& dispMat = _scene->GetMaterial(matId);
                        if (dispMat.displacementScale > 0.f && dispMat.displacementTexId >= 0) {
                            const auto* dispTex = _scene->GetTexture(dispMat.displacementTexId);
                            if (dispTex && dispTex->IsValid()) {
                                const int nPts = static_cast<int>(points.size());
                                const int nIdx = static_cast<int>(faceVertexIndices.size());

                                // Step 1: Compute per-vertex UVs by averaging face-varying UVs
                                VtVec2fArray vertexUVs(nPts, GfVec2f(0.f));
                                std::vector<int> uvCount(nPts, 0);
                                if (!uvs.empty() && uvsInterp == UsdGeomTokens->faceVarying) {
                                    for (int i = 0; i < nIdx; ++i) {
                                        int vi = faceVertexIndices[i];
                                        if (vi >= 0 && vi < nPts && i < static_cast<int>(uvs.size())) {
                                            vertexUVs[vi] += uvs[i];
                                            uvCount[vi]++;
                                        }
                                    }
                                    for (int i = 0; i < nPts; ++i) {
                                        if (uvCount[i] > 1) vertexUVs[i] /= float(uvCount[i]);
                                    }
                                } else if (!uvs.empty()) {
                                    // Vertex-interpolated UVs — direct mapping
                                    for (int i = 0; i < std::min(nPts, static_cast<int>(uvs.size())); ++i) {
                                        vertexUVs[i] = uvs[i];
                                    }
                                }

                                // Step 2: Sample displacement and offset vertices
                                float scale = dispMat.displacementScale;
                                float midpoint = dispMat.displacementMidpoint;
                                int texW = dispTex->GetWidth();
                                int texH = dispTex->GetHeight();
                                int texCh = dispTex->_channels;
                                const float* texPx = dispTex->_pixels.data();

                                for (int i = 0; i < nPts; ++i) {
                                    GfVec2f uv = vertexUVs[i];
                                    // Repeat wrap + bilinear sample
                                    float su = uv[0] - std::floor(uv[0]);
                                    float sv = 1.f - (uv[1] - std::floor(uv[1]));
                                    float fx = su * (texW - 1);
                                    float fy = sv * (texH - 1);
                                    int x0 = std::max(0, std::min(int(fx), texW-1));
                                    int y0 = std::max(0, std::min(int(fy), texH-1));
                                    int x1 = std::min(x0+1, texW-1);
                                    int y1 = std::min(y0+1, texH-1);
                                    float dx = fx - x0, dy = fy - y0;

                                    auto samplePx = [&](int x, int y) -> float {
                                        int idx = (y * texW + x) * texCh;
                                        return texPx[idx]; // red channel
                                    };
                                    auto samplePx3 = [&](int x, int y) -> GfVec3f {
                                        int idx = (y * texW + x) * texCh;
                                        float r = texPx[idx];
                                        float g = (texCh > 1) ? texPx[idx+1] : 0.f;
                                        float b = (texCh > 2) ? texPx[idx+2] : 0.f;
                                        return GfVec3f(r, g, b);
                                    };

                                    if (dispMat.displacementMode == 0) {
                                        // Scalar displacement: height along normal
                                        float c00 = samplePx(x0,y0), c10 = samplePx(x1,y0);
                                        float c01 = samplePx(x0,y1), c11 = samplePx(x1,y1);
                                        float val = (c00*(1-dx)+c10*dx)*(1-dy) + (c01*(1-dx)+c11*dx)*dy;
                                        float offset = (val - midpoint) * scale;
                                        GfVec3f N = normals[i];
                                        float nlen = N.GetLength();
                                        if (nlen > 1e-6f) N /= nlen;
                                        points[i] += N * offset;
                                    } else {
                                        // Vector displacement: RGB → XYZ offset
                                        GfVec3f c00 = samplePx3(x0,y0), c10 = samplePx3(x1,y0);
                                        GfVec3f c01 = samplePx3(x0,y1), c11 = samplePx3(x1,y1);
                                        GfVec3f val = (c00*(1-dx)+c10*dx)*(1-dy) + (c01*(1-dx)+c11*dx)*dy;
                                        GfVec3f offset = (val - GfVec3f(midpoint)) * scale;

                                        if (dispMat.displacementMode == 1) {
                                            // Tangent space: build tangent frame from normal
                                            GfVec3f N = normals[i].GetNormalized();
                                            GfVec3f up = (std::abs(N[1]) < 0.999f) ? GfVec3f(0,1,0) : GfVec3f(1,0,0);
                                            GfVec3f T = GfCross(up, N).GetNormalized();
                                            GfVec3f B = GfCross(N, T);
                                            points[i] += T * offset[0] + B * offset[1] + N * offset[2];
                                        } else {
                                            // Object space: RGB maps directly to XYZ
                                            points[i] += offset;
                                        }
                                    }
                                }

                                // Step 3: Recompute normals from displaced geometry
                                for (int i = 0; i < nPts; ++i) normals[i] = GfVec3f(0.f);
                                for (int i = 0; i + 2 < nIdx; i += 3) {
                                    int i0 = faceVertexIndices[i];
                                    int i1 = faceVertexIndices[i+1];
                                    int i2 = faceVertexIndices[i+2];
                                    if (i0 >= nPts || i1 >= nPts || i2 >= nPts) continue;
                                    GfVec3f e0 = points[i1] - points[i0];
                                    GfVec3f e1 = points[i2] - points[i0];
                                    GfVec3f fn = GfCross(e0, e1);
                                    normals[i0] += fn;
                                    normals[i1] += fn;
                                    normals[i2] += fn;
                                }
                                for (int i = 0; i < nPts; ++i) {
                                    float len = normals[i].GetLength();
                                    normals[i] = (len > 1e-8f) ? normals[i] / len : GfVec3f(0,1,0);
                                }

                                SLOG("SpectralRender: displaced %d vertices (scale=%.2f, tex=%dx%d) for %s\n",
                                        nPts, scale, texW, texH, prim.GetPath().GetText());
                            }
                        }
                    }

                    // Motion blur: pointsClose must match subdivided topology.
                    // For transform-only motion, the local points are identical
                    // at both times — just use the subdivided points for both.
                    // For vertex deformation blur, we'd need to subdivide both
                    // sets independently (future enhancement).
                    if (meshHasMotion) {
                        pointsClose = points;  // same refined local points
                    }
                }
            }
        }
#endif // SPECTRAL_HAS_OSD

        // World transform.
        //
        // Chain-walk path (Nuke node-graph input + motion blur enabled):
        //   The USG bridge baked every prim at the current frame so we can't
        //   trust xfCache at timeOpen/timeClose -- they all return the same
        //   snapshot. We already composed chainWorldOpen / chainWorldClose
        //   from upstream knob values read directly via get_value_at, which
        //   DO reflect animation. Use those.
        //
        // USD file / non-motion-blur path:
        //   xfCache with the relevant TimeCode works correctly.
        GfMatrix4d worldXf;
        if (useChainWalk) {
            worldXf = chainWorldOpen;
        } else if (motionBlurEnabled) {
            UsdGeomXformCache xfCacheOpen(timeOpen);
            worldXf = xfCacheOpen.GetLocalToWorldTransform(prim);
        } else {
            worldXf = xfCache.GetLocalToWorldTransform(prim);
        }
        GfMatrix4d normalXf = worldXf.GetInverse().GetTranspose();

        // Close-time transform for motion blur.
        GfMatrix4d worldXfClose = worldXf;
        if (meshHasMotion) {
            if (useChainWalk) {
                worldXfClose = chainWorldClose;
            } else {
                UsdGeomXformCache xfCacheClose(timeClose);
                worldXfClose = xfCacheClose.GetLocalToWorldTransform(prim);
            }
        }

        const int numPoints = static_cast<int>(points.size());

        SLOG("SpectralRender: mesh %s — %d points, %d faces, %d normals (%s)\n",
                prim.GetPath().GetText(), numPoints,
                static_cast<int>(faceVertexCounts.size()),
                static_cast<int>(normals.size()),
                normalsInterp.GetText());

        auto xfPoint = [&](const GfVec3f& p) -> GfVec3f {
            return GfVec3f(worldXf.Transform(GfVec3d(p)));
        };
        auto xfPointClose = [&](const GfVec3f& p) -> GfVec3f {
            return GfVec3f(worldXfClose.Transform(GfVec3d(p)));
        };
        auto xfNormal = [&](const GfVec3f& n) -> GfVec3f {
            GfVec3f xn = GfVec3f(normalXf.TransformDir(GfVec3d(n)));
            float len = xn.GetLength();
            return (len > 1e-6f) ? xn / len : GfVec3f(0.f, 1.f, 0.f);
        };

        pxr::SpectralMeshData data;
        data.id      = prim.GetPath();
        data.visible = true;
        data.objectId = _scene->NextObjectId();

        // Fan-triangulate each face
        int vertexOffset = 0;
        for (int fi = 0; fi < static_cast<int>(faceVertexCounts.size()); ++fi) {
            const int nv = faceVertexCounts[fi];
            if (nv < 3) {
                vertexOffset += nv;
                continue;
            }

            for (int ti = 0; ti < nv - 2; ++ti) {
                const int idx0 = vertexOffset;
                const int idx1 = vertexOffset + ti + 1;
                const int idx2 = vertexOffset + ti + 2;

                if (idx2 >= static_cast<int>(faceVertexIndices.size())) break;

                const int pi0 = faceVertexIndices[idx0];
                const int pi1 = faceVertexIndices[idx1];
                const int pi2 = faceVertexIndices[idx2];

                if (pi0 < 0 || pi0 >= numPoints ||
                    pi1 < 0 || pi1 >= numPoints ||
                    pi2 < 0 || pi2 >= numPoints) continue;

                pxr::SpectralTriangle tri;
                tri.v0 = xfPoint(points[pi0]);
                tri.v1 = xfPoint(points[pi1]);
                tri.v2 = xfPoint(points[pi2]);

                // Pref: object-space rest position (before displacement, no world xf)
                // Stays constant across animation — used for sticky texture projections
                const VtVec3fArray& refPts = pointsRef.empty() ? points : pointsRef;
                tri.pRef0 = refPts[pi0];
                tri.pRef1 = refPts[pi1];
                tri.pRef2 = refPts[pi2];

                // Motion blur: close-time positions
                if (meshHasMotion && !pointsClose.empty()) {
                    tri.v0_close = xfPointClose(pointsClose[pi0]);
                    tri.v1_close = xfPointClose(pointsClose[pi1]);
                    tri.v2_close = xfPointClose(pointsClose[pi2]);
                    tri.hasMotion = true;
                }

                GfVec3f e0 = tri.v1 - tri.v0;
                GfVec3f e1 = tri.v2 - tri.v0;
                GfVec3f fn = GfCross(e0, e1);
                float fl = fn.GetLength();
                tri.faceNormal = (fl > 1e-8f) ? fn / fl : GfVec3f(0.f, 1.f, 0.f);

                bool gotNormals = false;
                if (!normals.empty()) {
                    if (normalsInterp == UsdGeomTokens->faceVarying) {
                        if (idx2 < static_cast<int>(normals.size())) {
                            tri.n0 = xfNormal(normals[idx0]);
                            tri.n1 = xfNormal(normals[idx1]);
                            tri.n2 = xfNormal(normals[idx2]);
                            gotNormals = true;
                        }
                    } else if (normalsInterp == UsdGeomTokens->vertex) {
                        if (pi0 < static_cast<int>(normals.size()) &&
                            pi1 < static_cast<int>(normals.size()) &&
                            pi2 < static_cast<int>(normals.size())) {
                            tri.n0 = xfNormal(normals[pi0]);
                            tri.n1 = xfNormal(normals[pi1]);
                            tri.n2 = xfNormal(normals[pi2]);
                            gotNormals = true;
                        }
                    } else if (normalsInterp == UsdGeomTokens->uniform) {
                        if (fi < static_cast<int>(normals.size())) {
                            GfVec3f fn2 = xfNormal(normals[fi]);
                            tri.n0 = tri.n1 = tri.n2 = fn2;
                            gotNormals = true;
                        }
                    }
                }

                if (!gotNormals) {
                    tri.n0 = tri.n1 = tri.n2 = tri.faceNormal;
                }

                // SpectralMeshProperties normal-mode override: faceted
                // forces flat shading regardless of authored normals.
                if (meshPropsHasEntry && meshNormalMode == 2) {
                    tri.n0 = tri.n1 = tri.n2 = tri.faceNormal;
                }

                // Assign UVs
                tri.uv0 = tri.uv1 = tri.uv2 = GfVec2f(0.f);
                if (!uvs.empty()) {
                    if (uvsInterp == UsdGeomTokens->faceVarying) {
                        if (idx2 < static_cast<int>(uvs.size())) {
                            tri.uv0 = uvs[idx0];
                            tri.uv1 = uvs[idx1];
                            tri.uv2 = uvs[idx2];
                        }
                    } else if (uvsInterp == UsdGeomTokens->vertex) {
                        if (pi0 < static_cast<int>(uvs.size()) &&
                            pi1 < static_cast<int>(uvs.size()) &&
                            pi2 < static_cast<int>(uvs.size())) {
                            tri.uv0 = uvs[pi0];
                            tri.uv1 = uvs[pi1];
                            tri.uv2 = uvs[pi2];
                        }
                    }
                }

                tri.materialId = matId;
                tri.objectId = data.objectId;
                data.triangles.push_back(tri);
                totalTris++;
            }

            vertexOffset += nv;
        }

        if (!data.triangles.empty()) {
            // Project3D: recompute UVs by projecting vertices through camera
            if (!_projCameraVP.empty()) {
                for (auto& tri : data.triangles) {
                    auto it = _projCameraVP.find(tri.materialId);
                    if (it == _projCameraVP.end()) continue;
                    const GfMatrix4d& vp = it->second;
                    auto projectUV = [&](const GfVec3f& pos) -> GfVec2f {
                        // Row-vector multiplication: clip = point * VP (pxr convention)
                        double px = pos[0], py = pos[1], pz = pos[2];
                        double cx = px*vp[0][0] + py*vp[1][0] + pz*vp[2][0] + vp[3][0];
                        double cy = px*vp[0][1] + py*vp[1][1] + pz*vp[2][1] + vp[3][1];
                        double cw = px*vp[0][3] + py*vp[1][3] + pz*vp[2][3] + vp[3][3];
                        if (std::abs(cw) < 1e-8) return GfVec2f(0.f);
                        // format_xform already maps to UV space — just perspective divide
                        float u = float(cx / cw);
                        float v = float(cy / cw);
                        return GfVec2f(u, v);
                    };
                    tri.uv0 = projectUV(tri.v0);
                    tri.uv1 = projectUV(tri.v1);
                    tri.uv2 = projectUV(tri.v2);
                }
            }

            // Auto-smooth normals: average face normals per shared vertex
            // Gate:
            //   - Default: run when _scanlineCompat is on (original behavior)
            //   - SpectralMeshProperties normalMode=smooth (1): always run
            //   - SpectralMeshProperties normalMode=faceted (2): always skip
            //   - SpectralMeshProperties normalMode=vertex (3): always skip
            bool shouldSmooth = _scanlineCompat;
            if (meshPropsHasEntry) {
                if (meshNormalMode == 1) shouldSmooth = true;
                if (meshNormalMode == 2) shouldSmooth = false;
                if (meshNormalMode == 3) shouldSmooth = false;
            }
            if (shouldSmooth) {
                // Build vertex -> face normal accumulator using a
                // position-quantized hash. Using exact float equality
                // fails for meshes with "separate vertices" topology
                // (e.g. Nuke's GeoSphere in that mode) where adjacent
                // faces don't share indices; the supposedly-same
                // position can differ by a few ULPs due to different
                // transform paths, and the accumulator treats them as
                // distinct vertices -> smoothing never actually merges
                // across the seam, so the mesh stays faceted.
                //
                // Quantization tolerance: 1e-5 world units. Much smaller
                // than any visible feature in any reasonable scene scale,
                // comfortably larger than typical float drift.
                auto quantize = [](float f) -> int64_t {
                    return static_cast<int64_t>(std::llround(double(f) * 1.0e5));
                };
                struct QKey {
                    int64_t x, y, z;
                    bool operator==(const QKey& o) const {
                        return x == o.x && y == o.y && z == o.z;
                    }
                };
                struct QKeyHash {
                    size_t operator()(const QKey& k) const {
                        size_t h = 0;
                        h ^= std::hash<int64_t>{}(k.x) + 0x9e3779b9 + (h<<6) + (h>>2);
                        h ^= std::hash<int64_t>{}(k.y) + 0x9e3779b9 + (h<<6) + (h>>2);
                        h ^= std::hash<int64_t>{}(k.z) + 0x9e3779b9 + (h<<6) + (h>>2);
                        return h;
                    }
                };
                auto makeKey = [&](const pxr::GfVec3f& v) -> QKey {
                    return { quantize(v[0]), quantize(v[1]), quantize(v[2]) };
                };
                std::unordered_map<QKey, pxr::GfVec3f, QKeyHash> vertNormals;

                // Accumulate face normals
                for (const auto& tri : data.triangles) {
                    vertNormals[makeKey(tri.v0)] += tri.faceNormal;
                    vertNormals[makeKey(tri.v1)] += tri.faceNormal;
                    vertNormals[makeKey(tri.v2)] += tri.faceNormal;
                }

                // Normalize accumulated normals
                for (auto& kv : vertNormals) {
                    float len = kv.second.GetLength();
                    if (len > 1e-8f) kv.second /= len;
                }

                // Assign smooth normals (only where normals were face normals)
                for (auto& tri : data.triangles) {
                    // Check if all 3 normals are identical (face normal fallback)
                    if (tri.n0 == tri.n1 && tri.n1 == tri.n2) {
                        auto it0 = vertNormals.find(makeKey(tri.v0));
                        auto it1 = vertNormals.find(makeKey(tri.v1));
                        auto it2 = vertNormals.find(makeKey(tri.v2));
                        if (it0 != vertNormals.end()) tri.n0 = it0->second;
                        if (it1 != vertNormals.end()) tri.n1 = it1->second;
                        if (it2 != vertNormals.end()) tri.n2 = it2->second;
                    }
                }
            }

            // SpectralMeshProperties flip-normals: negate everything
            // (shading normals AND face normal) so backface-culling and
            // geometric orientation stay consistent. Runs last so it
            // applies to whichever normals the previous steps produced.
            if (meshPropsHasEntry && meshFlipNormals) {
                for (auto& tri : data.triangles) {
                    tri.n0 = -tri.n0;
                    tri.n1 = -tri.n1;
                    tri.n2 = -tri.n2;
                    tri.faceNormal = -tri.faceNormal;
                }
            }

            _scene->SetMeshData(data.id, std::move(data));
            meshCount++;
        }
    }

    SLOG("SpectralRender: loaded %d meshes, %d triangles total\n",
            meshCount, totalTris);

    // ------------------------------------------------------------------
    // Detect USD Volume prims (OpenVDBAsset) in the stage
    // ------------------------------------------------------------------
#ifdef SPECTRAL_HAS_VDB
    if (_volumes.empty()) {  // Only scan USD volumes if no SpectralVDBRead volumes found
        int masterMaxRes = _GetMasterMaxRes();
        for (const auto& prim : stage->Traverse()) {
            if (!prim.IsA<UsdVolVolume>()) continue;

            UsdVolVolume volumePrim(prim);
            if (!volumePrim) continue;

            std::string densityFile, tempFile, flameFile;
            std::string densityField, tempField, flameField;

            for (const auto& child : prim.GetChildren()) {
                if (!child.IsA<UsdVolOpenVDBAsset>()) continue;
                UsdVolOpenVDBAsset asset(child);
                if (!asset) continue;

                SdfAssetPath filePath;
                asset.GetFilePathAttr().Get(&filePath, timeCode);
                TfToken fieldName;
                asset.GetFieldNameAttr().Get(&fieldName, timeCode);

                std::string resolvedFile = filePath.GetResolvedPath();
                if (resolvedFile.empty()) resolvedFile = filePath.GetAssetPath();

                std::string fn = fieldName.GetString();
                if (fn == "density" || fn == "smoke" || fn == "soot" || fn == "scatter") {
                    densityFile = resolvedFile;
                    densityField = fn;
                } else if (fn == "temperature" || fn == "temp" || fn == "heat") {
                    tempFile = resolvedFile;
                    tempField = fn;
                } else if (fn == "flame" || fn == "flames" || fn == "fire" || fn == "fuel") {
                    flameFile = resolvedFile;
                    flameField = fn;
                } else if (densityFile.empty()) {
                    // First unknown field as density fallback
                    densityFile = resolvedFile;
                    densityField = fn;
                }
            }

            if (densityFile.empty()) continue;

            // Metadata-only load (~1ms) — just get bbox for transforms.
            // Actual voxel data is loaded in _EnsureFrameRendered at render resolution.
            auto vol = pxr::SpectralVDBLoader::LoadMetadataOnly(
                densityFile.c_str(),
                densityField.c_str());

            if (!vol || !vol->IsValid()) continue;

            // Apply world transform from USD stage
            GfMatrix4d xf = xfCache.GetLocalToWorldTransform(prim);
            GfVec3d trans = xf.ExtractTranslation();
            // Extract rotation (ZXY) and scale from matrix
            vol->translate = GfVec3f(float(trans[0]), float(trans[1]), float(trans[2]));

            // Extract scale from column lengths
            GfVec3d col0(xf[0][0], xf[1][0], xf[2][0]);
            GfVec3d col1(xf[0][1], xf[1][1], xf[2][1]);
            GfVec3d col2(xf[0][2], xf[1][2], xf[2][2]);
            float scx = float(col0.GetLength());
            float scy = float(col1.GetLength());
            float scz = float(col2.GetLength());
            vol->scale = GfVec3f(scx, scy, scz);

            // Extract rotation from normalized columns
            if (scx > 1e-6f && scy > 1e-6f && scz > 1e-6f) {
                float ry = std::asin(std::clamp(float(xf[0][2]) / scx, -1.f, 1.f));
                float cy = std::cos(ry);
                float rx = (std::abs(cy) > 1e-4f)
                    ? std::atan2(-float(xf[1][2]) / scy, float(xf[2][2]) / scz)
                    : 0.f;
                float rz = (std::abs(cy) > 1e-4f)
                    ? std::atan2(-float(xf[0][1]) / scx, float(xf[0][0]) / scx)
                    : 0.f;
                vol->rotate = GfVec3f(rx * 180.f / 3.14159265f,
                                      ry * 180.f / 3.14159265f,
                                      rz * 180.f / 3.14159265f);
            }

            vol->BuildTransform();
            _applyVolumeShading(vol);
            _volumes.push_back(vol);

            SLOG("SpectralRender: USD Volume '%s' → %s (%dx%dx%d) T=(%.1f,%.1f,%.1f)%s%s\n",
                prim.GetPath().GetText(), densityFile.c_str(),
                vol->resX, vol->resY, vol->resZ,
                vol->translate[0], vol->translate[1], vol->translate[2],
                !tempField.empty() ? " +temp" : "",
                !flameField.empty() ? " +flame" : "");
        }
        if (!_volumes.empty())
            _volume = _volumes[0];
    }
#endif

    // ------------------------------------------------------------------
    // Build camera from USD stage (may be overridden by _BuildCameraFromInput)
    // ------------------------------------------------------------------
    const Format* fmtPtr = &(info_.format());
    unsigned int W = 1920, H = 1080;
    if (fmtPtr) {
        W = static_cast<unsigned int>(fmtPtr->width());
        H = static_cast<unsigned int>(fmtPtr->height());
    }
    if (W == 0) W = 1920;
    if (H == 0) H = 1080;

    _camera.imageWidth  = W;
    _camera.imageHeight = H;
    _camera.pixelAspect = fmtPtr ? fmtPtr->pixel_aspect() : 1.0;
    const double aspect = double(W) / double(H);

    SLOG("SpectralRender: format %dx%d pixel_aspect=%.4f\n",
            W, H, _camera.pixelAspect);

    bool foundCamera = false;
    if (cameraPrim.IsValid()) {
        try {
            UsdGeomCamera geomCam(cameraPrim);
            GfCamera gfCam = geomCam.GetCamera(timeCode);
            GfMatrix4d camToWorld = xfCache.GetLocalToWorldTransform(cameraPrim);
            _camera.viewToWorld = camToWorld;
            GfFrustum frustum = gfCam.GetFrustum();
            GfMatrix4d projMatrix = frustum.ComputeProjectionMatrix();
            _camera.projInverse = projMatrix.GetInverse();
            foundCamera = true;

            GfVec3d camPos = camToWorld.ExtractTranslation();
            SLOG("SpectralRender: camera at (%.2f, %.2f, %.2f)\n",
                    camPos[0], camPos[1], camPos[2]);
        } catch (...) {
            SLOG("SpectralRender: camera extraction failed\n");
        }
    }

    if (!foundCamera) {
        // Default 24mm camera positioned to see a typical volume scene
        SLOG("SpectralRender: using default 24mm camera\n");
        // Position camera at (0, 2, 8) looking at origin
        GfMatrix4d lookAt(1.0);
        GfVec3d eye(0.0, 2.0, 8.0);
        GfVec3d target(0.0, 0.5, 0.0);
        GfVec3d up(0.0, 1.0, 0.0);
        GfVec3d fwd = (target - eye).GetNormalized();
        GfVec3d right = GfCross(fwd, up).GetNormalized();
        GfVec3d camUp = GfCross(right, fwd);
        lookAt[0][0]=right[0]; lookAt[0][1]=right[1]; lookAt[0][2]=right[2];
        lookAt[1][0]=camUp[0]; lookAt[1][1]=camUp[1]; lookAt[1][2]=camUp[2];
        lookAt[2][0]=-fwd[0];  lookAt[2][1]=-fwd[1];  lookAt[2][2]=-fwd[2];
        lookAt[3][0]=eye[0];   lookAt[3][1]=eye[1];   lookAt[3][2]=eye[2];
        _camera.viewToWorld = lookAt;
        _camera.focalLength = 24.f;

        // 24mm on 36mm sensor: hfov = 2*atan(18/24) = 73.7 deg
        const double hfov   = 2.0 * std::atan(18.0 / 24.0);
        const double near_  = 0.1, far_ = 10000.0;
        const double f      = 1.0 / std::tan(hfov * 0.5);
        GfMatrix4d proj(0.0);
        proj[0][0] = f;
        proj[1][1] = f * aspect;
        proj[2][2] = (far_ + near_) / (near_ - far_);
        proj[2][3] = -1.0;
        proj[3][2] = (2.0 * far_ * near_) / (near_ - far_);
        _camera.projInverse = proj.GetInverse();
    }
}

// ---------------------------------------------------------------------------
// _BuildCameraFromInput — override camera from input 0 (CameraOp) if connected
// ---------------------------------------------------------------------------
void SpectralRenderIop::_BuildCameraFromInput()
{
    _cameraFromInput = false;

    Op* camOp = (inputs() > 0) ? input(0) : nullptr;
    if (!camOp) return;

    CameraOp* cam = dynamic_cast<CameraOp*>(camOp);
    if (!cam) return;

    try {
        cam->validate(true);
    } catch (...) {
        SLOG("SpectralRender: camera input validation failed\n");
        return;
    }

    // Get image dimensions from info_ (already set by _validate from BG or format knob)
    const Format* fmtPtr = &(info_.format());
    unsigned int W = fmtPtr ? static_cast<unsigned int>(fmtPtr->width())  : 1920;
    unsigned int H = fmtPtr ? static_cast<unsigned int>(fmtPtr->height()) : 1080;
    if (W == 0) W = 1920;
    if (H == 0) H = 1080;
    const double aspect = double(W) / double(H);

    _camera.imageWidth  = W;
    _camera.imageHeight = H;
    _camera.pixelAspect = fmtPtr ? fmtPtr->pixel_aspect() : 1.0;
    // DDImage cam->matrix() is the camera-to-world matrix.
    // DDImage uses row-vector (v*M) and PXR Transform() also uses row-vector (v*M),
    // so viewToWorld is copied WITHOUT transposing.
    const DD::Image::Matrix4& cw = cam->matrix();
    _camera.viewToWorld = GfMatrix4d(
        cw[0][0], cw[0][1], cw[0][2], cw[0][3],
        cw[1][0], cw[1][1], cw[1][2], cw[1][3],
        cw[2][0], cw[2][1], cw[2][2], cw[2][3],
        cw[3][0], cw[3][1], cw[3][2], cw[3][3]
    );

    // Projection matrix — TRANSPOSE DDImage row-vector → PXR column-vector.
    // _MakeRay uses projInverse * vec4 (column-vector M*v multiply), so the
    // projection MUST be transposed from DDImage's row-vector convention.
    // DDImage's projection is square (proj[0][0] == proj[1][1]) — it only
    // encodes haperture. We must scale [1][1] by the image aspect ratio
    // so the vertical FOV matches the image height.
    const DD::Image::Matrix4& pr = cam->projection();
    GfMatrix4d pxrProj(
        pr[0][0], pr[1][0], pr[2][0], pr[3][0],
        pr[0][1], pr[1][1], pr[2][1], pr[3][1],
        pr[0][2], pr[1][2], pr[2][2], pr[3][2],
        pr[0][3], pr[1][3], pr[2][3], pr[3][3]
    );

    // Scale vertical projection by image aspect to correct for non-square image
    const double imageAspect = (double(W) * _camera.pixelAspect) / double(H);
    pxrProj[1][1] *= imageAspect;

    _camera.projInverse = pxrProj.GetInverse();

    // Read focal length from camera for DOF lens radius
    _camera.focalLength = static_cast<float>(cam->focal_length());

    _cameraFromInput = true;
    _camera.cameraMblur = false;  // TODO: camera motion blur (needs proper axis chain evaluation)

    GfVec3d camPos = _camera.viewToWorld.ExtractTranslation();
    SLOG("SpectralRender: camera from input at (%.2f, %.2f, %.2f)\n",
            camPos[0], camPos[1], camPos[2]);
    SLOG("SpectralRender: DDImage proj[0][0]=%.4f [1][1]=%.4f → corrected [1][1]=%.4f (aspect=%.4f)\n",
            pr[0][0], pr[1][1], pxrProj[1][1], imageAspect);
}

// ---------------------------------------------------------------------------
// _EnsureFrameRendered
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// _resolveFramePath — substitute frame number into VDB path
// ---------------------------------------------------------------------------
std::string SpectralRenderIop::_resolveFramePath(int frame) const
{
    std::string p(_vdbFile ? _vdbFile : "");
    if (p.empty()) return p;

    // #### padding
    size_t h = p.find('#');
    if (h != std::string::npos) {
        size_t he = h;
        while (he < p.size() && p[he] == '#') ++he;
        char buf[64];
        std::snprintf(buf, 64, "%0*d", int(he - h), frame);
        p.replace(h, he - h, buf);
        return p;
    }

    // %04d style
    size_t pc = p.find('%');
    if (pc != std::string::npos) {
        size_t s = pc + 1;
        if (s < p.size() && p[s] == '0') ++s;
        while (s < p.size() && p[s] >= '0' && p[s] <= '9') ++s;
        if (s < p.size() && p[s] == 'd') {
            char buf[64];
            std::snprintf(buf, 64, p.substr(pc, s - pc + 1).c_str(), frame);
            p.replace(pc, s - pc + 1, buf);
            return p;
        }
    }

    // Numeric suffix before .vdb
    size_t dot = p.rfind('.');
    if (dot == std::string::npos) dot = p.size();
    size_t ne = dot, ns = ne;
    while (ns > 0 && p[ns-1] >= '0' && p[ns-1] <= '9') --ns;
    if (ns < ne) {
        char buf[64];
        std::snprintf(buf, 64, "%0*d", int(ne - ns), frame);
        p.replace(ns, ne - ns, buf);
    }
    return p;
}

// ---------------------------------------------------------------------------
// append — include frame and VDB path in hash
// ---------------------------------------------------------------------------
void SpectralRenderIop::append(Hash& hash)
{
    // Iop::append auto-hashes every registered knob on this node plus the
    // output context (frame, view, proxy scale). This single line pre-empts
    // the entire class of "I toggle a knob but the frame doesn't refresh
    // until I scrub the timeline" bugs we've chased repeatedly -- any new
    // knob added to the node gets covered for free. See CLAUDE.md's
    // "Hash invalidation" section: this was technical debt #3 in the
    // roadmap and this is the fix.
    //
    // The manual hash.append() calls below remain as defensive double-
    // coverage AND to cover things Iop::append() can't see: registries
    // populated by other nodes (SpectralDraftingOp, SpectralShadowCatcherOp,
    // SpectralSurfaceOp, SpectralMeshPropertiesOp), internal state that
    // isn't knob-backed (_volumes.size, _shadowCatcherNames), and the
    // multi-hop scene graph walk below. Those must stay.
    //
    // Removed from this function: the local _wire* / _arch* / _pencil* /
    // _topo* / _wireAA* member hashes. Those members live on this Op but
    // are populated FROM the Drafting registry during _LoadFromPxrStage,
    // which runs after append(). Hashing them here was either redundant
    // (registry hash below already covers the authoritative values) or
    // actively stale (append() sees last-render's state). The registry
    // hash is the single source of truth; its expanded coverage below
    // was the real fix.
    Iop::append(hash);

    hash.append(outputContext().frame());
    hash.append(_vdbFrameOffset);
    // Knobs that change the rendered output but aren't on any input chain,
    // so they need to be hashed explicitly. Without these, toggling the
    // knob dirties the viewport preview but not the rendered frame, so
    // the user has to scrub time to force a re-render.
    hash.append(_scanlineCompat ? 1 : 0);
    hash.append(_useBuiltinLight ? 1 : 0);
    hash.append(_deviceMode);
    if (_vdbFile) hash.append(_vdbFile);
    hash.append((int)_volumes.size());
    hash.append(_edgeSamples);
    if (_shadowCatcherNames) hash.append(_shadowCatcherNames);
    // Hash wireframe registry — every field that affects output. The
    // previous version only hashed thickness/opacity/style/nth, so changing
    // the pencil or topo knobs on a SpectralWireframe node would update the
    // registry but fail to invalidate the Iop's cached frame until the user
    // scrubbed the timeline. Classic CLAUDE.md gotcha #1.
    {
        const auto& wireReg = SpectralDraftingOp::GetRegistry();
        hash.append((int)wireReg.size());
        for (const auto& kv : wireReg) {
            const auto& p = kv.second;
            hash.append(kv.first.c_str());
            hash.append(p.thickness); hash.append(p.opacity);
            hash.append(p.color[0]); hash.append(p.color[1]); hash.append(p.color[2]);
            hash.append(p.dashed ? 1 : 0);
            hash.append(p.dashLength); hash.append(p.gapLength);
            hash.append(p.nth); hash.append(p.style);
            hash.append(p.gridDensity);
            hash.append(p.showTriangles ? 1 : 0);
            hash.append(p.archSilhouetteWeight);
            hash.append(p.archMediumWeight);
            hash.append(p.archThinWeight);
            hash.append(p.archSilhouetteColor[0]);
            hash.append(p.archSilhouetteColor[1]);
            hash.append(p.archSilhouetteColor[2]);
            hash.append(p.archThinOpacity);
            hash.append(p.pencilWobble);
            hash.append(p.pencilPressure);
            hash.append(p.pencilCrossHatch ? 1 : 0);
            hash.append(p.pencilHatchDensity);
            hash.append(p.pencilHatchAngle);
            hash.append(p.topoDirection);
            hash.append(p.topoUpVector[0]);
            hash.append(p.topoUpVector[1]);
            hash.append(p.topoUpVector[2]);
            hash.append(p.topoContourInterval);
            hash.append(p.topoMajorEvery);
            hash.append(p.aaMode);
            hash.append(p.aaWidth);
        }
    }
    // Hash shadow catcher registry
    {
        const auto& scReg = SpectralShadowCatcherOp::GetRegistry();
        hash.append((int)scReg.size());
        for (const auto& kv : scReg) {
            hash.append(kv.first.c_str());
            hash.append(kv.second.shadowIntensity);
        }
    }
    // Hash mesh-properties registry. Size changes (e.g. a node being
    // disabled and removing its entry) plus the per-entry knob values
    // both need to land in the hash so toggling disable or tweaking the
    // node re-renders immediately on the current frame.
    {
        const auto& mpReg = SpectralMeshPropertiesOp::GetRegistry();
        hash.append((int)mpReg.size());
        for (const auto& kv : mpReg) {
            hash.append(kv.first.c_str());
            hash.append(kv.second.subdivLevel);
            hash.append(kv.second.subdivScheme);
            hash.append(kv.second.normalMode);
            hash.append(kv.second.flipNormals ? 1 : 0);
            hash.append(kv.second.useDisplayColor ? 1 : 0);
            hash.append(kv.second.displayColor[0]);
            hash.append(kv.second.displayColor[1]);
            hash.append(kv.second.displayColor[2]);
            hash.append(kv.second.displayOpacity);
            hash.append(kv.second.visible ? 1 : 0);
        }
    }
    // Hash camera input (input 0) — re-render when camera moves or changes
    Op* cam = (inputs() > 0) ? input(0) : nullptr;
    if (cam) {
        cam->validate(false);
        hash.append(cam->hash());
    }
    // Hash scn input connection + disabled state
    Op* scn = (inputs() > 1) ? input(1) : nullptr;
    if (scn) {
        hash.append(scn->hash());
        hash.append(scn->node_disabled() ? 1 : 0);
    } else {
        hash.append(0);
    }
    // Hash SpectralSurface version — any knob change bumps this
    extern int GetSpectralSurfaceVersion();
    hash.append(GetSpectralSurfaceVersion());
    // Hash SpectralSurface registry — ensures material changes trigger re-render
    const auto& registry = SpectralSurfaceOp::GetRegistry();
    hash.append((int)registry.size());
    for (const auto& kv : registry) {
        hash.append(kv.first.c_str());
        const auto& p = kv.second;
        hash.append(p.abbeNumber);
        hash.append(p.thinFilmThickness);
        hash.append(p.metalType);
        hash.append(p.textureBlend);
        hash.append(p.absorptionColor[0]); hash.append(p.absorptionColor[1]); hash.append(p.absorptionColor[2]);
        hash.append(p.absorptionDensity);
        hash.append(p.gratingSpacing); hash.append(p.gratingStrength);
        hash.append(p.fluorAbsorb); hash.append(p.fluorEmit); hash.append(p.fluorStrength);
        hash.append(p.sssColor[0]); hash.append(p.sssColor[1]); hash.append(p.sssColor[2]);
        hash.append(p.sssRadius);
        hash.append(p.mapMode); hash.append(p.dispType);
        hash.append(p.bumpStrength);
        hash.append(p.displacementScale); hash.append(p.displacementMidpoint);
    }
    // Walk scn input chain to hash all connected ops (catches ShaderOp changes)
    if (scn) {
        std::vector<Op*> toWalk;
        toWalk.push_back(scn);
        for (int w = 0; w < (int)toWalk.size() && w < 32; ++w) {
            Op* cur = toWalk[w];
            if (!cur) continue;
            hash.append(cur->hash());
            for (int inp = 0; inp < cur->inputs() && inp < 8; ++inp)
                if (cur->input(inp)) toWalk.push_back(cur->input(inp));
        }
    }
}

// ---------------------------------------------------------------------------
// build_handles / draw_handle — wireframe bbox + point cloud in 3D viewport
// ---------------------------------------------------------------------------
void SpectralRenderIop::build_handles(ViewerContext* ctx)
{
    if (ctx->transform_mode() == VIEWER_2D) return;
    if (node_disabled()) return;

    // Propagate scn input (input 1) so Hydra sees upstream Volume/Geo prims
    // This enables HdStorm fog rendering when VDBRead is in volume display mode
    Op* scn = (inputs() > 1) ? input(1) : nullptr;
    if (scn) scn->build_handles(ctx);

    // Local GL preview (point cloud + bbox + env light dome + geometry wireframe)
    {
        bool hasVolume = _vdb3dPreview && (!_volumes.empty() || (_volume && _volume->HasBbox()));
        bool hasLights = _vdb3dPreview && (_cachedEnvLight || _cachedStudioLight);
        bool hasGeo = _scene && _scene->TotalTriangles() > 0;
        if (hasVolume || hasLights || hasGeo) add_draw_handle(ctx);
    }

    // Camera handles
    Op* cam = (inputs() > 0) ? input(0) : nullptr;
    if (cam) cam->build_handles(ctx);
}

// ---------------------------------------------------------------------------
// GL Volume Ray March Shader
// ---------------------------------------------------------------------------
static const char* kVolVS = R"(
#version 330 compatibility
varying vec3 vWorldPos;
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    vWorldPos = gl_Vertex.xyz;
}
)";

static const char* kVolFS = R"(
#version 330 compatibility
uniform sampler3D uDensity;
uniform sampler3D uTemp;
uniform vec3 uBboxMin;
uniform vec3 uBboxMax;
uniform float uDensityMult;
uniform float uTempMult;
uniform int uMaxSteps;
uniform vec3 uLightDir;
uniform vec3 uLightCol;
uniform vec3 uAmbient;
// Volume transform
uniform float uHasXform;
uniform vec3 uVolCenter;
uniform mat3 uInvRotM;
uniform vec3 uInvScale;
uniform vec3 uOrigMin;
uniform vec3 uOrigMax;
varying vec3 vWorldPos;

vec3 worldToUVW(vec3 p) {
    if (uHasXform > 0.5) {
        vec3 local = p - uVolCenter;
        vec3 unrot = uInvRotM * local;
        vec3 unscaled = unrot * uInvScale;
        vec3 origCenter = (uOrigMin + uOrigMax) * 0.5;
        vec3 origHalf = (uOrigMax - uOrigMin) * 0.5;
        vec3 orig = unscaled + origCenter;
        return (orig - uOrigMin) / (origHalf * 2.0);
    } else {
        return (p - uBboxMin) / (uBboxMax - uBboxMin);
    }
}

void main() {
    vec3 camPos = (gl_ModelViewMatrixInverse * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    vec3 rd = normalize(vWorldPos - camPos);
    vec3 size = uBboxMax - uBboxMin;
    float diag = length(size);
    float step = diag / 128.0;

    vec3 inv = 1.0 / rd;
    vec3 t0 = (uBboxMin - camPos) * inv;
    vec3 t1 = (uBboxMax - camPos) * inv;
    vec3 mn = min(t0, t1);
    vec3 mx = max(t0, t1);
    float tNear = max(max(mn.x, mn.y), max(mn.z, 0.0));
    float tFar  = min(min(mx.x, mx.y), mx.z);
    if (tNear >= tFar) discard;

    vec3 lightDir = normalize(uLightDir);

    float transmittance = 1.0;
    vec3 color = vec3(0.0);
    for (int i = 0; i < 128; i++) {
        float t = tNear + float(i) * step;
        if (t > tFar) break;

        vec3 p = camPos + rd * t;
        vec3 uvw = worldToUVW(p);

        // Skip if outside [0,1]³ (rotated volume doesn't fill axis-aligned bbox)
        if (uvw.x < 0.0 || uvw.x > 1.0 || uvw.y < 0.0 || uvw.y > 1.0 || uvw.z < 0.0 || uvw.z > 1.0) continue;

        float d = texture3D(uDensity, uvw).r * uDensityMult;
        float tmp = texture3D(uTemp, uvw).r * uTempMult;
        if (d < 0.02) continue;

        vec3 emit = vec3(0.0);
        if (tmp > 0.01) {
            emit.r = min(1.0, tmp * 3.0);
            emit.g = max(0.0, min(1.0, (tmp - 0.15) * 2.5));
            emit.b = max(0.0, min(1.0, (tmp - 0.5) * 3.0));
            emit *= tmp * 2.0;
        }

        // Shadow in UVW space
        float shadowD = 0.0;
        vec3 shadowStep = worldToUVW(p + lightDir * diag * 0.04) - uvw;
        for (int s = 1; s <= 6; s++) {
            vec3 sp = uvw + shadowStep * float(s);
            if (sp.x >= 0.0 && sp.x <= 1.0 && sp.y >= 0.0 && sp.y <= 1.0 && sp.z >= 0.0 && sp.z <= 1.0)
                shadowD += texture3D(uDensity, sp).r * uDensityMult;
        }
        float shadow = exp(-shadowD * 1.2);

        vec3 Li = uLightCol * shadow * 0.3 + uAmbient + emit;
        float sigma = d * step * 0.6;
        float tr = exp(-sigma);
        color += transmittance * Li * (1.0 - tr);
        transmittance *= tr;
        if (transmittance < 0.01) break;
    }
    float alpha = 1.0 - transmittance;
    if (alpha < 0.005) discard;
    gl_FragColor = vec4(color / max(alpha, 0.001), alpha);
}
)";

// Ground shadow shader — marches through volume from ground toward light
static const char* kShadowVS = R"(
#version 330 compatibility
varying vec3 vWorldPos;
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    vWorldPos = gl_Vertex.xyz;
}
)";

static const char* kShadowFS = R"(
#version 330 compatibility
uniform sampler3D uDensity;
uniform vec3 uBboxMin;
uniform vec3 uBboxMax;
uniform float uDensityMult;
uniform vec3 uLightDir;
varying vec3 vWorldPos;

void main() {
    // Ray from ground point toward light — intersect volume bbox
    vec3 size = uBboxMax - uBboxMin;
    vec3 ld = normalize(uLightDir);
    vec3 inv = 1.0 / ld;
    vec3 t0 = (uBboxMin - vWorldPos) * inv;
    vec3 t1 = (uBboxMax - vWorldPos) * inv;
    vec3 mn = min(t0, t1);
    vec3 mx = max(t0, t1);
    float tNear = max(max(mn.x, mn.y), max(mn.z, 0.0));
    float tFar  = min(min(mx.x, mx.y), mx.z);
    if (tNear >= tFar) discard;

    float totalD = 0.0;
    float step = (tFar - tNear) / 24.0;
    for (int i = 0; i < 24; i++) {
        float t = tNear + float(i) * step;
        vec3 p = vWorldPos + ld * t;
        vec3 uvw = (p - uBboxMin) / size;
        totalD += texture3D(uDensity, uvw).r * uDensityMult;
    }
    float shadow = 1.0 - exp(-totalD * 0.4);
    if (shadow < 0.02) discard;
    gl_FragColor = vec4(0.0, 0.0, 0.0, shadow * 0.35);
}
)";

// Geometry shading shader — flat-shaded triangles with volume shadow
static const char* kGeoVS = R"(
#version 330 compatibility
varying vec3 vWorldPos;
varying vec3 vNormal;
varying vec2 vTexCoord;
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    vWorldPos = gl_Vertex.xyz;
    vNormal = gl_Normal;
    vTexCoord = gl_MultiTexCoord0.xy;
}
)";

static const char* kGeoFS = R"(
#version 330 compatibility
uniform sampler3D uDensity;
uniform sampler2D uShadowMap;
uniform vec3 uBboxMin;
uniform vec3 uBboxMax;
uniform float uDensityMult;
uniform vec3 uLightDir;
uniform vec3 uLightCol;
uniform vec3 uAmbient;
uniform vec3 uMatColor;
uniform float uMetallic;
uniform float uRoughness;
uniform float uHasVolume;
uniform float uHasVolXform;
uniform vec3 uVolCenter;
uniform vec3 uVolInvScale;
uniform vec3 uVolOrigMin;
uniform vec3 uVolOrigMax;
uniform mat3 uVolInvRotM;
uniform float uHasShadowMap;
uniform float uHasEnvRefl;
uniform float uHasGeoRefl;
uniform sampler2D uReflColorTex;
uniform sampler2D uReflDepthTex;
uniform mat4 uMVP;
uniform mat4 uReflMVP;
uniform mat4 uLightVP;
uniform float uShadowTexelSize;
uniform float uShadowSoftness;
uniform int uShadowPCFRadius;
uniform int uVolShadowSamples;
uniform int uConstantMode;
uniform sampler2D uBaseColorTex;
uniform int uHasBaseColorTex;
varying vec3 vWorldPos;
varying vec3 vNormal;
varying vec2 vTexCoord;

// Environment sky dome with sun disc for reflections
vec3 envColor(vec3 dir, vec3 sunDir, vec3 sunCol) {
    float y = dir.y;

    // Sky gradient
    vec3 skyZenith = uAmbient * 2.5 + vec3(0.06, 0.1, 0.22);
    vec3 skyHorizon = sunCol * 0.1 + vec3(0.12, 0.1, 0.08);
    vec3 ground = vec3(0.03, 0.025, 0.02);

    vec3 sky;
    if (y > 0.0) {
        float t = pow(max(0.0, y), 0.6);
        sky = mix(skyHorizon, skyZenith, t);
        // Horizon glow near sun azimuth
        float sunProx = max(0.0, dot(normalize(vec3(dir.x, 0.0, dir.z)),
                                      normalize(vec3(sunDir.x, 0.0, sunDir.z) + vec3(0.001))));
        sky += sunCol * 0.05 * pow(sunProx, 8.0) * (1.0 - t);
    } else {
        sky = mix(skyHorizon * 0.4, ground, min(1.0, -y * 3.0));
    }

    // Sun disc — subtle, not overpowering
    float sunAngle = max(0.0, dot(dir, sunDir));
    float corona = pow(sunAngle, 128.0) * 0.5 + pow(sunAngle, 16.0) * 0.1;
    sky += sunCol * corona;

    return sky;
}

void main() {
    // ScanlineRender compat: no lights = constant shader (flat colour)
    if (uConstantMode > 0) {
        vec3 col = uMatColor;
        float alpha = 1.0;
        if (uHasBaseColorTex > 0) {
            vec4 texSample = texture2D(uBaseColorTex, vTexCoord);
            col = texSample.rgb;
            alpha = texSample.a;
        }
        if (alpha < 0.01) discard;
        gl_FragColor = vec4(col * alpha, alpha);
        return;
    }

    vec3 N = normalize(vNormal);
    vec3 V = normalize((gl_ModelViewMatrixInverse * vec4(0.0,0.0,0.0,1.0)).xyz - vWorldPos);
    if (dot(N, V) < 0.0) N = -N;

    vec3 L = normalize(uLightDir);
    float NdL = max(dot(N, L), 0.0);
    float NdV = max(dot(N, V), 0.001);

    // Reflection vector
    vec3 R = reflect(-V, N);

    // Schlick Fresnel
    float F0base = 0.04;
    float F0 = mix(F0base, 1.0, uMetallic);
    float cosTheta = max(dot(V, N), 0.0);
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);

    // GGX specular with Smith geometry term (properly energy-conserving)
    vec3 H = normalize(V + L);
    float NdH = max(dot(N, H), 0.0);
    float rough = max(0.04, uRoughness);
    float alpha2 = rough * rough * rough * rough;
    float denom = NdH * NdH * (alpha2 - 1.0) + 1.0;
    float D = alpha2 / (3.14159 * denom * denom);
    // Smith G1 (GGX)
    float G1V = 2.0 * NdV / (NdV + sqrt(alpha2 + (1.0 - alpha2) * NdV * NdV));
    float G1L = 2.0 * NdL / (NdL + sqrt(alpha2 + (1.0 - alpha2) * NdL * NdL));
    float G = G1V * G1L;
    float spec = min(D * G * fresnel / max(4.0 * NdV * NdL, 0.001), 16.0);

    // Geometry shadow map
    float geoShadow = 1.0;
    if (uHasShadowMap > 0.5) {
        vec4 lightClip = uLightVP * vec4(vWorldPos, 1.0);
        vec3 lightNDC = lightClip.xyz / lightClip.w;
        vec2 shadowUV = lightNDC.xy * 0.5 + 0.5;
        float fragDepth = lightNDC.z * 0.5 + 0.5;

        if (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 && shadowUV.y >= 0.0 && shadowUV.y <= 1.0) {
            float pcfStep = uShadowTexelSize * (1.0 + uShadowSoftness * 15.0);
            if (uShadowPCFRadius == 0) {
                float mapDepth = texture2D(uShadowMap, shadowUV).r;
                geoShadow = (fragDepth - 0.002 > mapDepth) ? 0.0 : 1.0;
            } else {
                float shadow = 0.0;
                float count = 0.0;
                for (int y = -3; y <= 3; y++) {
                    if (y < -uShadowPCFRadius || y > uShadowPCFRadius) continue;
                    for (int x = -3; x <= 3; x++) {
                        if (x < -uShadowPCFRadius || x > uShadowPCFRadius) continue;
                        float mapDepth = texture2D(uShadowMap, shadowUV + vec2(float(x), float(y)) * pcfStep).r;
                        shadow += (fragDepth - 0.002 > mapDepth) ? 0.0 : 1.0;
                        count += 1.0;
                    }
                }
                geoShadow = shadow / max(count, 1.0);
            }
        }
    }

    // Volume shadow on geometry
    float volShadow = 1.0;
    if (uHasVolume > 0.5 && uVolShadowSamples > 0) {
        vec3 size = uBboxMax - uBboxMin;
        vec3 ld = normalize(uLightDir);
        vec3 inv = vec3(
            abs(ld.x) > 0.0001 ? 1.0/ld.x : 1e6,
            abs(ld.y) > 0.0001 ? 1.0/ld.y : 1e6,
            abs(ld.z) > 0.0001 ? 1.0/ld.z : 1e6
        );
        vec3 t0 = (uBboxMin - vWorldPos) * inv;
        vec3 t1 = (uBboxMax - vWorldPos) * inv;
        vec3 mn = min(t0, t1);
        vec3 mx = max(t0, t1);
        float tNear = max(max(mn.x, mn.y), mn.z);
        float tFar  = min(min(mx.x, mx.y), mx.z);
        tNear = max(tNear, 0.001);
        if (tNear < tFar && tFar > 0.0) {
            float totalD = 0.0;
            float marchStep = (tFar - tNear) / float(uVolShadowSamples);
            for (int i = 0; i < 32; i++) {
                if (i >= uVolShadowSamples) break;
                float t = tNear + (float(i) + 0.5) * marchStep;
                vec3 p = vWorldPos + ld * t;
                // Convert world position to volume UVW (with transform support)
                vec3 uvw;
                if (uHasVolXform > 0.5) {
                    vec3 local = p - uVolCenter;
                    vec3 unrot = uVolInvRotM * local;
                    vec3 unscaled = unrot * uVolInvScale;
                    vec3 origSize = uVolOrigMax - uVolOrigMin;
                    vec3 origCenter = (uVolOrigMin + uVolOrigMax) * 0.5;
                    uvw = (unscaled + origCenter - uVolOrigMin) / origSize;
                } else {
                    uvw = (p - uBboxMin) / size;
                }
                if (uvw.x >= 0.0 && uvw.x <= 1.0 && uvw.y >= 0.0 && uvw.y <= 1.0 && uvw.z >= 0.0 && uvw.z <= 1.0)
                    totalD += texture3D(uDensity, uvw).r * uDensityMult;
            }
            volShadow = exp(-totalD * 3.0);
        }
    }

    float shadow = min(geoShadow, volShadow);

    // Diffuse (reduced by metallic — metals don't diffuse)
    vec3 diffuse = uMatColor * (1.0 - uMetallic) * uLightCol * NdL * shadow;

    // Hemisphere ambient: sky fill stronger on surfaces facing up, dimmer facing down
    float hemiBlend = N.y * 0.5 + 0.5;  // 1=facing sky, 0.5=horizontal, 0=facing ground
    vec3 skyAmb = uAmbient * (0.4 + 0.6 * hemiBlend);  // sky contribution
    vec3 groundAmb = uAmbient * 0.2 * (1.0 - hemiBlend);  // ground bounce
    vec3 ambient = uMatColor * (1.0 - uMetallic) * (skyAmb + groundAmb);

    // Environment reflection with sun disc
    vec3 refl = vec3(0.0);
    if (uHasEnvRefl > 0.5) {
        vec3 envSharp = envColor(R, L, uLightCol);
        // Rough surfaces blur the reflection toward normal direction
        vec3 envBlur = envColor(N, L, uLightCol);
        float blur = uRoughness * uRoughness;
        refl = mix(envSharp, envBlur, blur);
    }

    // Metallic tints reflections with base color, dielectric is white
    vec3 specColor = mix(vec3(1.0), uMatColor, uMetallic);

    // Geometry reflection — reflected camera pre-pass lookup with roughness blur
    if (uHasGeoRefl > 0.5 && rough < 0.5) {
        // Project fragment through reflected camera to find reflection UV
        vec4 reflClip = uReflMVP * vec4(vWorldPos, 1.0);
        if (reflClip.w > 0.01) {
            vec2 reflUV = reflClip.xy / reflClip.w * 0.5 + 0.5;

            if (reflUV.x > 0.02 && reflUV.x < 0.98 && reflUV.y > 0.02 && reflUV.y < 0.98) {
                // Roughness blur: 3x3 sample grid
                float blurR = rough * rough * 0.05;
                vec3 accumCol = vec3(0.0);
                float accumA = 0.0;
                float totalW = 0.0;

                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        vec2 sUV = reflUV + vec2(float(dx), float(dy)) * blurR;
                        if (sUV.x < 0.01 || sUV.x > 0.99 || sUV.y < 0.01 || sUV.y > 0.99) continue;
                        float sd = texture2D(uReflDepthTex, sUV).r;
                        if (sd >= 0.9999) continue;
                        vec4 sc = texture2D(uReflColorTex, sUV);
                        float w = (dx == 0 && dy == 0) ? 1.0 : 0.5;
                        accumCol += sc.rgb * sc.a * w;
                        accumA += sc.a * w;
                        totalW += w;
                    }
                }

                if (accumA > 0.01) {
                    vec3 reflColor = accumCol / accumA;
                    float coverage = accumA / max(totalW, 1.0);
                    float edgeFade = min(1.0, reflUV.x * 20.0) * min(1.0, (1.0 - reflUV.x) * 20.0)
                                   * min(1.0, reflUV.y * 20.0) * min(1.0, (1.0 - reflUV.y) * 20.0);
                    float roughFade = 1.0 - rough * 2.0;
                    refl = mix(refl, reflColor, clamp(edgeFade * roughFade * coverage * 0.6, 0.0, 0.6));
                }
            }
        }
    }

    // Specular: direct highlight + env reflection (clamped to prevent fireflies)
    vec3 specular = specColor * min(spec * uLightCol * shadow + refl * fresnel, vec3(8.0));

    gl_FragColor = vec4(diffuse + ambient + specular, 1.0);
}
)";

// Shadow map depth pass — renders geometry from light POV
static const char* kShadowDepthVS = R"(
#version 330 compatibility
uniform mat4 uLightVP;
void main() {
    gl_Position = uLightVP * gl_Vertex;
}
)";

static const char* kShadowDepthFS = R"(
#version 330 compatibility
void main() {
    // depth is written automatically
}
)";

static GLuint _CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
        fprintf(stderr, "SpectralRender: GL shader error: %s\n", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

void SpectralRenderIop::_InitGLVolShader()
{
    if (_glVolProg) return;
    GLuint vs = _CompileShader(GL_VERTEX_SHADER, kVolVS);
    GLuint fs = _CompileShader(GL_FRAGMENT_SHADER, kVolFS);
    if (!vs || !fs) { if(vs) glDeleteShader(vs); if(fs) glDeleteShader(fs); return; }
    _glVolProg = glCreateProgram();
    glAttachShader(_glVolProg, vs); glAttachShader(_glVolProg, fs);
    glLinkProgram(_glVolProg);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok; glGetProgramiv(_glVolProg, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(_glVolProg, 512, nullptr, log);
        fprintf(stderr, "SpectralRender: GL link error: %s\n", log);
        glDeleteProgram(_glVolProg); _glVolProg = 0;
    }

    // Shadow shader
    if (!_glShadowProg) {
        GLuint svs = _CompileShader(GL_VERTEX_SHADER, kShadowVS);
        GLuint sfs = _CompileShader(GL_FRAGMENT_SHADER, kShadowFS);
        if (svs && sfs) {
            _glShadowProg = glCreateProgram();
            glAttachShader(_glShadowProg, svs); glAttachShader(_glShadowProg, sfs);
            glLinkProgram(_glShadowProg);
            glDeleteShader(svs); glDeleteShader(sfs);
            GLint sok; glGetProgramiv(_glShadowProg, GL_LINK_STATUS, &sok);
            if (!sok) { glDeleteProgram(_glShadowProg); _glShadowProg = 0; }
        } else {
            if (svs) glDeleteShader(svs); if (sfs) glDeleteShader(sfs);
        }
    }

    // Geometry shading shader
    if (!_glGeoProg) {
        GLuint gvs = _CompileShader(GL_VERTEX_SHADER, kGeoVS);
        GLuint gfs = _CompileShader(GL_FRAGMENT_SHADER, kGeoFS);
        if (gvs && gfs) {
            _glGeoProg = glCreateProgram();
            glAttachShader(_glGeoProg, gvs); glAttachShader(_glGeoProg, gfs);
            glLinkProgram(_glGeoProg);
            glDeleteShader(gvs); glDeleteShader(gfs);
            GLint gok; glGetProgramiv(_glGeoProg, GL_LINK_STATUS, &gok);
            if (!gok) { glDeleteProgram(_glGeoProg); _glGeoProg = 0; }
        } else {
            if (gvs) glDeleteShader(gvs); if (gfs) glDeleteShader(gfs);
        }
    }

    // Shadow map depth program
    if (!_glShadowDepthProg) {
        GLuint dvs = _CompileShader(GL_VERTEX_SHADER, kShadowDepthVS);
        GLuint dfs = _CompileShader(GL_FRAGMENT_SHADER, kShadowDepthFS);
        if (dvs && dfs) {
            _glShadowDepthProg = glCreateProgram();
            glAttachShader(_glShadowDepthProg, dvs); glAttachShader(_glShadowDepthProg, dfs);
            glLinkProgram(_glShadowDepthProg);
            glDeleteShader(dvs); glDeleteShader(dfs);
            GLint dok; glGetProgramiv(_glShadowDepthProg, GL_LINK_STATUS, &dok);
            if (!dok) { glDeleteProgram(_glShadowDepthProg); _glShadowDepthProg = 0; }
        } else {
            if (dvs) glDeleteShader(dvs); if (dfs) glDeleteShader(dfs);
        }
    }

    // Shadow map FBO + depth texture (fixed 1024x1024)
    if (!_glShadowFBO && _glShadowDepthProg) {
        glGenTextures(1, &_glShadowDepthTex);
        glBindTexture(GL_TEXTURE_2D, _glShadowDepthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kShadowMapSize, kShadowMapSize,
                     0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float borderColor[] = {1.f, 1.f, 1.f, 1.f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &_glShadowFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, _glShadowFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _glShadowDepthTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "SpectralRender: shadow FBO incomplete: 0x%x\n", status);
            glDeleteFramebuffers(1, &_glShadowFBO); _glShadowFBO = 0;
            glDeleteTextures(1, &_glShadowDepthTex); _glShadowDepthTex = 0;
        }
        _glShadowMapCurSize = kShadowMapSize;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void SpectralRenderIop::_UploadGLVolTex(const pxr::SpectralVolume* vol)
{
    if (!vol) return;
    int rX = vol->resX, rY = vol->resY, rZ = vol->resZ;
    // Always re-upload: data can change between viewport preview and render loads

    // Compute max density for normalization
    _glVolMaxDensity = 0.001f;
    _glVolMaxTemp = 0.001f;
    if (!vol->density.empty()) {
        for (size_t i = 0; i < vol->density.size(); i++)
            if (vol->density[i] > _glVolMaxDensity) _glVolMaxDensity = vol->density[i];
    }
    if (!vol->temperature.empty()) {
        for (size_t i = 0; i < vol->temperature.size(); i++)
            if (vol->temperature[i] > _glVolMaxTemp) _glVolMaxTemp = vol->temperature[i];
    }

    // Density texture
    if (!_glVolDensityTex) glGenTextures(1, &_glVolDensityTex);
    glBindTexture(GL_TEXTURE_3D, _glVolDensityTex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
    if (!vol->density.empty())
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, rX, rY, rZ, 0, GL_RED, GL_FLOAT, vol->density.data());
    else {
        std::vector<float> zeros(rX*rY*rZ, 0.f);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, rX, rY, rZ, 0, GL_RED, GL_FLOAT, zeros.data());
    }

    // Temperature texture
    if (!_glVolTempTex) glGenTextures(1, &_glVolTempTex);
    glBindTexture(GL_TEXTURE_3D, _glVolTempTex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
    if (!vol->temperature.empty())
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, rX, rY, rZ, 0, GL_RED, GL_FLOAT, vol->temperature.data());
    else {
        std::vector<float> zeros(rX*rY*rZ, 0.f);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, rX, rY, rZ, 0, GL_RED, GL_FLOAT, zeros.data());
    }

    glBindTexture(GL_TEXTURE_3D, 0);
    _glVolTexResX = rX; _glVolTexResY = rY; _glVolTexResZ = rZ;
    _glVolTexFrame = (int)outputContext().frame();
}

// Forward declarations (defined in _BuildLightRig section)
static void sunColorFromElevation(double elev, double turbidity, double& r, double& g, double& b);
static void skyColorFromElevation(double elev, double turbidity, double& r, double& g, double& b);

void SpectralRenderIop::_DrawVolumeShaded(ViewerContext* ctx, bool writeDepth)
{
    if (_volumes.empty()) return;
    auto& vol = _volumes[0];
    if (!vol || !vol->IsValid()) return;

    _InitGLVolShader();
    if (!_glVolProg) { fprintf(stderr, "SpectralRender: GL vol shader failed to compile\n"); return; }

    _UploadGLVolTex(vol.get());
    if (!_glVolDensityTex) { fprintf(stderr, "SpectralRender: GL vol texture upload failed\n"); return; }

    pxr::GfVec3f bMin = vol->GetBboxMin();
    pxr::GfVec3f bMax = vol->GetBboxMax();

    // Compute light direction and color from scene lights
    float lightDirX = 0.f, lightDirY = 1.f, lightDirZ = 0.f;
    float lightR = 0.f, lightG = 0.f, lightB = 0.f;
    float ambR = 0.f, ambG = 0.f, ambB = 0.f;

    bool vpHasLight = _cachedEnvLight || _useBuiltinLight;
    if (vpHasLight) {
        double sunElev = _sunElevation, sunAz = _sunAzimuth;
        double sunInt = _sunIntensity, turbidity = _turbidity;
        double skyInt = _skyIntensity;
        if (_cachedEnvLight) {
            sunElev = _cachedEnvLight->sunElevation;
            sunAz = _cachedEnvLight->sunAzimuth;
            sunInt = _cachedEnvLight->sunIntensity;
            skyInt = _cachedEnvLight->skyIntensity;
        }

        // Sun direction: negate dirFromElevAzim (shader marches toward light)
        {
            double er = sunElev * M_PI / 180.0, ar = sunAz * M_PI / 180.0;
            lightDirX = float(-std::cos(er) * std::sin(ar));
            lightDirY = float(std::sin(er));
            lightDirZ = float(std::cos(er) * std::cos(ar));
            float len = std::sqrt(lightDirX*lightDirX + lightDirY*lightDirY + lightDirZ*lightDirZ);
            if (len > 1e-6f) { lightDirX /= len; lightDirY /= len; lightDirZ /= len; }
        }

        // Sun color from elevation — match render intensity curve (sunInt² / pi)
        {
            double r, g, b;
            sunColorFromElevation(sunElev, turbidity, r, g, b);
            double elevFactor = std::max(0.1, std::min(sunElev / 12.0, 1.0));
            float si = float(sunInt * sunInt * elevFactor * 0.32);  // 1/pi ≈ 0.32
            lightR = float(r * si); lightG = float(g * si); lightB = float(b * si);
        }

        // Sky ambient from elevation, scaled by sky fill intensity
        // Lower multiplier to match render — viewport ambient is flat, render uses proper GI
        {
            double r, g, b;
            skyColorFromElevation(sunElev, turbidity, r, g, b);
            float skyMul = float(std::min(skyInt * 0.12, 1.5));
            ambR = float(r * skyMul); ambG = float(g * skyMul); ambB = float(b * skyMul);
        }
    }

    // Save GL state
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    GLint prevProg; glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(writeDepth ? GL_TRUE : GL_FALSE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    glUseProgram(_glVolProg);

    // Uniforms (no matrix uniforms needed — GLSL 120 uses gl_ModelViewProjectionMatrix)
    glUniform3f(glGetUniformLocation(_glVolProg, "uBboxMin"), bMin[0], bMin[1], bMin[2]);
    glUniform3f(glGetUniformLocation(_glVolProg, "uBboxMax"), bMax[0], bMax[1], bMax[2]);
    // Normalize density to 0-1 range in shader, then apply artistic mult
    glUniform1f(glGetUniformLocation(_glVolProg, "uDensityMult"), 1.f / _glVolMaxDensity);
    glUniform1f(glGetUniformLocation(_glVolProg, "uTempMult"),
                _glVolMaxTemp > 0.01f ? 1.f / _glVolMaxTemp : 1.f);
    glUniform1i(glGetUniformLocation(_glVolProg, "uMaxSteps"), 128);
    glUniform3f(glGetUniformLocation(_glVolProg, "uLightDir"), lightDirX, lightDirY, lightDirZ);
    glUniform3f(glGetUniformLocation(_glVolProg, "uLightCol"), lightR, lightG, lightB);
    glUniform3f(glGetUniformLocation(_glVolProg, "uAmbient"), ambR, ambG, ambB);

    // Volume transform uniforms
    glUniform1f(glGetUniformLocation(_glVolProg, "uHasXform"), vol->hasTransform ? 1.f : 0.f);
    if (vol->hasTransform) {
        glUniform3f(glGetUniformLocation(_glVolProg, "uVolCenter"), vol->_center[0], vol->_center[1], vol->_center[2]);
        glUniform3f(glGetUniformLocation(_glVolProg, "uInvScale"), vol->_invScale[0], vol->_invScale[1], vol->_invScale[2]);
        glUniform3f(glGetUniformLocation(_glVolProg, "uOrigMin"), vol->bboxMin[0], vol->bboxMin[1], vol->bboxMin[2]);
        glUniform3f(glGetUniformLocation(_glVolProg, "uOrigMax"), vol->bboxMax[0], vol->bboxMax[1], vol->bboxMax[2]);
        // Inverse rotation: _rotM is row-major, GL interprets as column-major = transpose = inverse
        glUniformMatrix3fv(glGetUniformLocation(_glVolProg, "uInvRotM"), 1, GL_FALSE, vol->_rotM);
    }

    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, _glVolDensityTex);
    glUniform1i(glGetUniformLocation(_glVolProg, "uDensity"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, _glVolTempTex);
    glUniform1i(glGetUniformLocation(_glVolProg, "uTemp"), 1);

    // Draw bbox cube with glBegin/glEnd (compatible with Nuke's GL context)
    float x0=bMin[0], y0=bMin[1], z0=bMin[2];
    float x1=bMax[0], y1=bMax[1], z1=bMax[2];
    glBegin(GL_TRIANGLES);
    // Front (+Z)
    glVertex3f(x0,y0,z1); glVertex3f(x1,y0,z1); glVertex3f(x1,y1,z1);
    glVertex3f(x0,y0,z1); glVertex3f(x1,y1,z1); glVertex3f(x0,y1,z1);
    // Back (-Z)
    glVertex3f(x1,y0,z0); glVertex3f(x0,y0,z0); glVertex3f(x0,y1,z0);
    glVertex3f(x1,y0,z0); glVertex3f(x0,y1,z0); glVertex3f(x1,y1,z0);
    // Left (-X)
    glVertex3f(x0,y0,z0); glVertex3f(x0,y0,z1); glVertex3f(x0,y1,z1);
    glVertex3f(x0,y0,z0); glVertex3f(x0,y1,z1); glVertex3f(x0,y1,z0);
    // Right (+X)
    glVertex3f(x1,y0,z1); glVertex3f(x1,y0,z0); glVertex3f(x1,y1,z0);
    glVertex3f(x1,y0,z1); glVertex3f(x1,y1,z0); glVertex3f(x1,y1,z1);
    // Top (+Y)
    glVertex3f(x0,y1,z1); glVertex3f(x1,y1,z1); glVertex3f(x1,y1,z0);
    glVertex3f(x0,y1,z1); glVertex3f(x1,y1,z0); glVertex3f(x0,y1,z0);
    // Bottom (-Y)
    glVertex3f(x0,y0,z0); glVertex3f(x1,y0,z0); glVertex3f(x1,y0,z1);
    glVertex3f(x0,y0,z0); glVertex3f(x1,y0,z1); glVertex3f(x0,y0,z1);
    glEnd();

    // Restore
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_3D, 0);
    glUseProgram(prevProg);
    glPopAttrib();
}

void SpectralRenderIop::draw_handle(ViewerContext* ctx)
{
    glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT | GL_ENABLE_BIT | GL_POINT_BIT);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);

    float kPi = 3.14159265f;

    // ─── Volume bbox wireframe (all volumes) ──────────────────────
    for (size_t vi = 0; vi < _volumes.size(); ++vi) {
        auto& vol = _volumes[vi];
        if (!vol || !vol->HasBbox()) continue;

        // Use transformed bbox for wireframe (matches render)
        pxr::GfVec3f bMin = vol->GetBboxMin();
        pxr::GfVec3f bMax = vol->GetBboxMax();

        if (_vdbShowBbox) {
            float x0=bMin[0], y0=bMin[1], z0=bMin[2];
            float x1=bMax[0], y1=bMax[1], z1=bMax[2];
            float hue = float(vi) * 0.3f;
            glColor4f(0.4f+hue*0.2f, 0.6f, 0.4f+hue*0.1f, 0.35f);
            glLineWidth(1.f);
            glBegin(GL_LINE_LOOP); glVertex3f(x0,y0,z0);glVertex3f(x1,y0,z0);glVertex3f(x1,y1,z0);glVertex3f(x0,y1,z0);glEnd();
            glBegin(GL_LINE_LOOP); glVertex3f(x0,y0,z1);glVertex3f(x1,y0,z1);glVertex3f(x1,y1,z1);glVertex3f(x0,y1,z1);glEnd();
            glBegin(GL_LINES);
            glVertex3f(x0,y0,z0);glVertex3f(x0,y0,z1);
            glVertex3f(x1,y0,z0);glVertex3f(x1,y0,z1);
            glVertex3f(x1,y1,z0);glVertex3f(x1,y1,z1);
            glVertex3f(x0,y1,z0);glVertex3f(x0,y1,z1);
            glEnd();
        }
    }

    // ─── Point cloud (ALL volumes, built once) ──────────────────────
    if (_vdbShowPoints && !_vdbIsMetadataOnly && !_volumes.empty()) {
        if (_vdbPreviewDirty || _vdbPreviewPoints.empty()) {
            _vdbPreviewPoints.clear(); _vdbMaxDensity = 1e-6f;
            _vdbMaxTemp = 1e-6f; _vdbMaxFlame = 1e-6f;

            int totalBudget = 20000;  // total points across all volumes
            int maxPointsPerVol = std::max(500, totalBudget / std::max(1, (int)_volumes.size()));
            for (size_t vj = 0; vj < _volumes.size(); ++vj) {
                auto& v = _volumes[vj];
                if (!v || !v->IsValid()) continue;
                bool hasTemp = !v->temperature.empty();
                bool hasFlame = !v->flame.empty();
                int totalVoxels = v->resX * v->resY * v->resZ;
                int step = std::max(1, (int)std::cbrt(double(totalVoxels) / maxPointsPerVol));
                pxr::GfVec3f vMin = v->bboxMin, vSz = v->bboxMax - v->bboxMin;
                pxr::GfVec3f vCenter = (v->bboxMin + v->bboxMax) * 0.5f;
                int volPts = 0;
                for (int iz = 0; iz < v->resZ; iz += step)
                    for (int iy = 0; iy < v->resY; iy += step)
                        for (int ix = 0; ix < v->resX; ix += step) {
                            float u = float(ix)/v->resX, uv = float(iy)/v->resY, w = float(iz)/v->resZ;
                            float d = v->SampleDensity(u, uv, w);
                            float temp = hasTemp ? v->SampleTemperature(u, uv, w) : 0.f;
                            float fl = hasFlame ? v->SampleFlame(u, uv, w) : 0.f;
                            if (d < 0.01f && temp < 0.01f && fl < 0.01f) continue;
                            if (d > _vdbMaxDensity) _vdbMaxDensity = d;
                            if (temp > _vdbMaxTemp) _vdbMaxTemp = temp;
                            if (fl > _vdbMaxFlame) _vdbMaxFlame = fl;
                            pxr::GfVec3f localP(vMin[0]+u*vSz[0], vMin[1]+uv*vSz[1], vMin[2]+w*vSz[2]);
                            pxr::GfVec3f worldP = localP;
                            if (v->hasTransform) {
                                pxr::GfVec3f rel = localP - vCenter;
                                pxr::GfVec3f scaled(rel[0]*v->scale[0], rel[1]*v->scale[1], rel[2]*v->scale[2]);
                                worldP = v->_center + pxr::GfVec3f(
                                    v->_rotM[0]*scaled[0] + v->_rotM[1]*scaled[1] + v->_rotM[2]*scaled[2],
                                    v->_rotM[3]*scaled[0] + v->_rotM[4]*scaled[1] + v->_rotM[5]*scaled[2],
                                    v->_rotM[6]*scaled[0] + v->_rotM[7]*scaled[1] + v->_rotM[8]*scaled[2]);
                            }
                            VDBPreviewPoint pt;
                            pt.x = worldP[0]; pt.y = worldP[1]; pt.z = worldP[2];
                            pt.density = d; pt.temperature = temp; pt.flame = fl;
                            _vdbPreviewPoints.push_back(pt);
                            volPts++;
                        }
                SLOG("SpectralRender: viewport vol[%zu] %dx%dx%d step=%d pts=%d xf=%d bbox(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                        vj, v->resX, v->resY, v->resZ, step, volPts,
                        v->hasTransform ? 1 : 0,
                        v->GetBboxMin()[0], v->GetBboxMin()[1], v->GetBboxMin()[2],
                        v->GetBboxMax()[0], v->GetBboxMax()[1], v->GetBboxMax()[2]);
            }
            SLOG("SpectralRender: viewport total %d points from %d volumes\n",
                    (int)_vdbPreviewPoints.size(), (int)_volumes.size());
            _vdbPreviewDirty = false;
        }
        if (!_vdbPreviewPoints.empty()) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glPointSize(float(_vdbPointSize)); glEnable(GL_POINT_SMOOTH);
            float invD = 1.f / _vdbMaxDensity;
            float invT = 1.f / _vdbMaxTemp;
            float invF = 1.f / _vdbMaxFlame;
            glBegin(GL_POINTS);
            for (const auto& pt : _vdbPreviewPoints) {
                float t = std::min(pt.density * invD, 1.f);
                float temp = std::min(pt.temperature * invT, 1.f);
                float fl = std::min(pt.flame * invF, 1.f);
                float r, g, b, a;
                switch (_vdbRenderMode) {
                    case 1: r = g = b = t; a = .15f+.85f*t; break;  // Greyscale
                    case 2: {  // Heat — use temperature if available
                        float h = (pt.temperature > 0.01f) ? temp : t;
                        r = std::min(h*3.f, 1.f);
                        g = std::max(0.f, std::min((h-.33f)*3.f, 1.f));
                        b = std::max(0.f, std::min((h-.66f)*3.f, 1.f));
                        a = .15f+.85f*std::max(t, h);
                    } break;
                    case 3: {  // Cool
                        r = std::max(0.f, std::min((t-.5f)*2.f, 1.f));
                        g = std::max(0.f, std::min((t-.25f)*2.f, 1.f));
                        b = std::min(t*2.f, 1.f);
                        a = .15f+.85f*t;
                    } break;
                    case 4: {  // Blackbody — temperature to physical fire color
                        float h = (pt.temperature > 0.01f) ? temp : t;
                        // Blackbody approximation: dark red → orange → yellow → white
                        r = std::min(1.f, h * 3.f);
                        g = std::max(0.f, std::min(1.f, (h - 0.15f) * 2.5f));
                        b = std::max(0.f, std::min(1.f, (h - 0.5f) * 3.f));
                        // Boost with flame channel emission
                        float emis = (pt.flame > 0.01f) ? fl * 0.5f : 0.f;
                        r = std::min(1.f, r + emis);
                        g = std::min(1.f, g + emis * 0.6f);
                        a = .1f + .9f * std::max(t, std::max(h, fl));
                    } break;
                    case 5: {  // Explosion — fire core + smoke shell
                        float h = (pt.temperature > 0.01f) ? temp : t;
                        float f = (pt.flame > 0.01f) ? fl : h;
                        // Fire core: bright orange-yellow from flame/temperature
                        float fireR = std::min(1.f, f * 2.5f);
                        float fireG = std::max(0.f, std::min(1.f, (f - 0.1f) * 2.f));
                        float fireB = std::max(0.f, std::min(1.f, (f - 0.6f) * 3.f));
                        // Smoke: grey-brown from density without temperature
                        float smokeV = t * 0.4f;
                        // Blend: high temp = fire, low temp = smoke
                        float fireMix = std::max(h, f);
                        r = fireR * fireMix + smokeV * (1.f - fireMix);
                        g = fireG * fireMix + smokeV * 0.9f * (1.f - fireMix);
                        b = fireB * fireMix + smokeV * 0.7f * (1.f - fireMix);
                        a = .1f + .9f * std::max(t, fireMix);
                    } break;
                    default: {  // Lit (mode 0) — warm tint, use temperature when available
                        if (pt.temperature > 0.01f) {
                            // Warm: density for opacity, temperature for color
                            float h = temp;
                            r = std::min(1.f, h * 2.5f + t * 0.3f);
                            g = std::max(0.f, std::min(1.f, (h - 0.15f) * 2.f + t * 0.2f));
                            b = std::max(0.f, std::min(1.f, (h - 0.5f) * 2.f));
                        } else {
                            r = t; g = t * 0.9f; b = t * 0.7f;
                        }
                        a = .15f+.85f*t;
                    } break;
                }
                glColor4f(r, g, b, a);
                glVertex3f(pt.x, pt.y, pt.z);
            }
            glEnd(); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }

    // ─── Light preview (subtle, professional) ────────────────────────

    // Environment light
    if (_cachedEnvLight && _cachedEnvLight->skyPreset > 0) {
        float R = float(_cachedEnvLight->domeRadius);
        float guideOp = float(_cachedEnvLight->guideOpacity);
        if (_cachedEnvLight->showDome) {
            float dR=float(_cachedEnvLight->guideDomeColor[0]), dG=float(_cachedEnvLight->guideDomeColor[1]), dB=float(_cachedEnvLight->guideDomeColor[2]);
            float lw = float(_cachedEnvLight->guideLineWidth);
            // Ground horizon circle — bright
            glColor4f(dR*1.2f, dG*1.2f, dB*1.2f, 0.8f*guideOp); glLineWidth(lw*2.f);
            glBegin(GL_LINE_LOOP);
            for (int i=0;i<64;++i){float a=float(i)/64.f*2.f*kPi;glVertex3f(R*std::cos(a),0,R*std::sin(a));}
            glEnd();
            // 45deg latitude
            glColor4f(dR, dG, dB, (0.6f)*guideOp); glLineWidth(lw*1.5f);
            float elR45 = 45.f*kPi/180.f;
            float rr45 = R*std::cos(elR45), y45 = R*std::sin(elR45);
            glBegin(GL_LINE_LOOP);
            for (int i=0;i<48;++i){float a=float(i)/48.f*2.f*kPi;glVertex3f(rr45*std::cos(a),y45,rr45*std::sin(a));}
            glEnd();
            // 30deg latitude
            float elR30 = 30.f*kPi/180.f;
            float rr30 = R*std::cos(elR30), y30 = R*std::sin(elR30);
            glColor4f(dR, dG, dB, (0.4f)*guideOp);
            glBegin(GL_LINE_LOOP);
            for (int i=0;i<48;++i){float a=float(i)/48.f*2.f*kPi;glVertex3f(rr30*std::cos(a),y30,rr30*std::sin(a));}
            glEnd();
            // Zenith cross
            glColor4f(dR, dG, dB, (0.3f)*guideOp); glLineWidth(lw);
            float zr = R*0.08f;
            glBegin(GL_LINES);
            glVertex3f(-zr,R,0); glVertex3f(zr,R,0);
            glVertex3f(0,R,-zr); glVertex3f(0,R,zr);
            glEnd();
        }
        if (_cachedEnvLight->showSunArrow) {
            float sunI = std::min(1.f, float(_cachedEnvLight->sunIntensity) / 5.f);
            float elR = float(_cachedEnvLight->sunElevation)*kPi/180.f;
            float azR = float(_cachedEnvLight->sunAzimuth)*kPi/180.f;
            // Sun position: toward-sun direction (matches shadow/shading light dir)
            float sx=-R*std::cos(elR)*std::sin(azR), sy=R*std::sin(elR), sz=R*std::cos(elR)*std::cos(azR);

            // ─── Sun path arc (day arc) ───────────────────────────
            // Dashed arc from east through zenith to west at current azimuth plane
            glColor4f(float(_cachedEnvLight->guideArcColor[0]), float(_cachedEnvLight->guideArcColor[1]), float(_cachedEnvLight->guideArcColor[2]), (0.12f + sunI * 0.08f)*guideOp);
            glLineWidth(float(_cachedEnvLight->guideLineWidth));
            int arcSegs = 64;
            int dashSkip = (_cachedEnvLight->guideDashPattern == 0) ? 999 : (_cachedEnvLight->guideDashPattern == 2) ? 2 : 3;
            for (int i = 0; i < arcSegs; i++) {
                if (i % dashSkip >= (dashSkip-1)) continue;
                float e0 = float(i) / float(arcSegs) * kPi;      // 0 to pi (horizon to horizon)
                float e1 = float(i+1) / float(arcSegs) * kPi;
                glBegin(GL_LINES);
                glVertex3f(-R*std::cos(e0)*std::sin(azR), R*std::sin(e0), R*std::cos(e0)*std::cos(azR));
                glVertex3f(-R*std::cos(e1)*std::sin(azR), R*std::sin(e1), R*std::cos(e1)*std::cos(azR));
                glEnd();
            }
            // Horizon ticks at arc endpoints
            float htk = R * 0.04f;
            glBegin(GL_LINES);
            glVertex3f(-R*std::sin(azR)-htk*std::cos(azR), 0, R*std::cos(azR)+htk*std::sin(azR));
            glVertex3f(-R*std::sin(azR)+htk*std::cos(azR), 0, R*std::cos(azR)-htk*std::sin(azR));
            glVertex3f(R*std::sin(azR)-htk*std::cos(azR), 0, -R*std::cos(azR)+htk*std::sin(azR));
            glVertex3f(R*std::sin(azR)+htk*std::cos(azR), 0, -R*std::cos(azR)-htk*std::sin(azR));
            glEnd();

            // Thin line from origin to sun
            glColor4f(float(_cachedEnvLight->guideSunColor[0]), float(_cachedEnvLight->guideSunColor[1]), float(_cachedEnvLight->guideSunColor[2]), (0.15f+sunI*0.2f)*guideOp);
            glLineWidth(float(_cachedEnvLight->guideLineWidth));
            glBegin(GL_LINES);glVertex3f(0,0,0);glVertex3f(sx,sy,sz);glEnd();

            // ─── Ra sun icon ──────────────────────────────────────
            // Billboard: compute right/up vectors facing camera
            float mvInv[16];
            {
                float mv[16];
                glGetFloatv(GL_MODELVIEW_MATRIX, mv);
                // Extract camera position from modelview (inv translation)
                // For billboard we just need the view direction from sun
                // Camera pos = -R^T * t where R is upper-left 3x3, t is column 3
                float cx2 = -(mv[0]*mv[12]+mv[1]*mv[13]+mv[2]*mv[14]);
                float cy2 = -(mv[4]*mv[12]+mv[5]*mv[13]+mv[6]*mv[14]);
                float cz2 = -(mv[8]*mv[12]+mv[9]*mv[13]+mv[10]*mv[14]);
                mvInv[0]=cx2; mvInv[1]=cy2; mvInv[2]=cz2;
            }
            float cx=mvInv[0], cy=mvInv[1], cz=mvInv[2];
            // View direction from sun to camera
            float vx=cx-sx, vy=cy-sy, vz=cz-sz;
            float vlen=std::sqrt(vx*vx+vy*vy+vz*vz);
            if (vlen>1e-6f){vx/=vlen;vy/=vlen;vz/=vlen;}
            // Right = normalize(cross(up, view))
            float ux=0,uy=1,uz=0;
            if(std::abs(vy)>0.99f){ux=1;uy=0;uz=0;}
            float rx=uy*vz-uz*vy, ry=uz*vx-ux*vz, rz=ux*vy-uy*vx;
            float rlen=std::sqrt(rx*rx+ry*ry+rz*rz);
            if(rlen>1e-6f){rx/=rlen;ry/=rlen;rz/=rlen;}
            // Up = cross(view, right)
            float ux2=vy*rz-vz*ry, uy2=vz*rx-vx*rz, uz2=vx*ry-vy*rx;

            float sunR0 = R * 0.035f * float(_cachedEnvLight->guideIconScale);  // inner disk radius
            float sunR1 = R * 0.065f * float(_cachedEnvLight->guideIconScale);  // ray tip radius
            float alpha = (0.6f + sunI * 0.3f) * guideOp;

            // Inner disk (filled triangle fan)
            glColor4f(float(_cachedEnvLight->guideSunColor[0]), float(_cachedEnvLight->guideSunColor[1]), float(_cachedEnvLight->guideSunColor[2]), alpha);
            int diskSegs = 16;
            glBegin(GL_TRIANGLE_FAN);
            glVertex3f(sx, sy, sz);  // center
            for (int i = 0; i <= diskSegs; i++) {
                float a = float(i) / float(diskSegs) * 2.f * kPi;
                float px = sx + (rx*std::cos(a) + ux2*std::sin(a)) * sunR0;
                float py = sy + (ry*std::cos(a) + uy2*std::sin(a)) * sunR0;
                float pz = sz + (rz*std::cos(a) + uz2*std::sin(a)) * sunR0;
                glVertex3f(px, py, pz);
            }
            glEnd();

            // Outer ring
            glColor4f(float(_cachedEnvLight->guideSunColor[0])*0.9f, float(_cachedEnvLight->guideSunColor[1])*0.8f, float(_cachedEnvLight->guideSunColor[2])*0.7f, alpha * 0.6f);
            glLineWidth(float(_cachedEnvLight->guideLineWidth) * 1.5f);
            float ringR = sunR0 * 1.3f;
            glBegin(GL_LINE_LOOP);
            for (int i = 0; i < 24; i++) {
                float a = float(i) / 24.f * 2.f * kPi;
                float px = sx + (rx*std::cos(a) + ux2*std::sin(a)) * ringR;
                float py = sy + (ry*std::cos(a) + uy2*std::sin(a)) * ringR;
                float pz = sz + (rz*std::cos(a) + uz2*std::sin(a)) * ringR;
                glVertex3f(px, py, pz);
            }
            glEnd();

            // Radiating rays (12 rays, alternating long/short like Ra's crown)
            glColor4f(float(_cachedEnvLight->guideSunColor[0]), float(_cachedEnvLight->guideSunColor[1])*0.95f, float(_cachedEnvLight->guideSunColor[2])*1.3f, alpha * 0.8f);
            glLineWidth(float(_cachedEnvLight->guideLineWidth) * 1.5f);
            int numRays = 12;
            for (int i = 0; i < numRays; i++) {
                float a = float(i) / float(numRays) * 2.f * kPi;
                float rayLen = (i % 2 == 0) ? sunR1 : sunR1 * 0.7f;
                float inner = sunR0 * 1.1f;
                float ix = sx + (rx*std::cos(a) + ux2*std::sin(a)) * inner;
                float iy = sy + (ry*std::cos(a) + uy2*std::sin(a)) * inner;
                float iz = sz + (rz*std::cos(a) + uz2*std::sin(a)) * inner;
                float ox = sx + (rx*std::cos(a) + ux2*std::sin(a)) * rayLen;
                float oy = sy + (ry*std::cos(a) + uy2*std::sin(a)) * rayLen;
                float oz = sz + (rz*std::cos(a) + uz2*std::sin(a)) * rayLen;
                glBegin(GL_LINES);glVertex3f(ix,iy,iz);glVertex3f(ox,oy,oz);glEnd();
            }
        }
        if (_cachedEnvLight->showCompass) {
            float cr = R*1.05f, ci = R*0.95f;
            glColor4f(0.8f, 0.3f, 0.3f, 0.35f); glLineWidth(1.f);
            glBegin(GL_LINES);glVertex3f(0,0,ci);glVertex3f(0,0,cr);glEnd();
            glColor4f(0.45f, 0.45f, 0.45f, 0.2f);
            glBegin(GL_LINES);
            glVertex3f(0,0,-ci);glVertex3f(0,0,-cr);
            glVertex3f(ci,0,0);glVertex3f(cr,0,0);
            glVertex3f(-ci,0,0);glVertex3f(-cr,0,0);
            glEnd();
        }
    }

    // Studio light
    if (_cachedStudioLight && _cachedStudioLight->mix > 0.01) {
        float R = float(_cachedStudioLight->rigRadius);
        float eR = float(_cachedStudioLight->keyElevation)*kPi/180.f;
        float aR = float(_cachedStudioLight->keyAzimuth)*kPi/180.f;
        float kx=R*std::cos(eR)*std::sin(aR), ky=R*std::sin(eR), kz=R*std::cos(eR)*std::cos(aR);
        float fA=aR+kPi, fE=eR*0.3f;
        float fx=R*0.8f*std::cos(fE)*std::sin(fA), fy=R*0.8f*std::sin(fE), fz=R*0.8f*std::cos(fE)*std::cos(fA);
        float rA=aR+kPi, rE=eR*0.8f;
        float rx=R*std::cos(rE)*std::sin(rA), ry=R*std::sin(rE), rz=R*std::cos(rE)*std::cos(rA);
        float m = float(_cachedStudioLight->mix);

        if (_cachedStudioLight->showLights) {
            // Key — small warm dot + thin line
            glColor4f(_cachedStudioLight->keyColor[0],_cachedStudioLight->keyColor[1],_cachedStudioLight->keyColor[2],0.6f*m);
            glPointSize(6.f); glBegin(GL_POINTS);glVertex3f(kx,ky,kz);glEnd();
            glColor4f(_cachedStudioLight->keyColor[0],_cachedStudioLight->keyColor[1],_cachedStudioLight->keyColor[2],0.12f*m);
            glLineWidth(1.f); glBegin(GL_LINES);glVertex3f(kx,ky,kz);glVertex3f(0,0,0);glEnd();
            // Fill — smaller cool dot
            glColor4f(_cachedStudioLight->fillColor[0],_cachedStudioLight->fillColor[1],_cachedStudioLight->fillColor[2],0.4f*m);
            glPointSize(4.f); glBegin(GL_POINTS);glVertex3f(fx,fy,fz);glEnd();
            glColor4f(_cachedStudioLight->fillColor[0],_cachedStudioLight->fillColor[1],_cachedStudioLight->fillColor[2],0.08f*m);
            glBegin(GL_LINES);glVertex3f(fx,fy,fz);glVertex3f(0,0,0);glEnd();
            // Rim — small white dot
            glColor4f(_cachedStudioLight->rimColor[0],_cachedStudioLight->rimColor[1],_cachedStudioLight->rimColor[2],0.4f*m);
            glPointSize(4.f); glBegin(GL_POINTS);glVertex3f(rx,ry,rz);glEnd();
            glColor4f(_cachedStudioLight->rimColor[0],_cachedStudioLight->rimColor[1],_cachedStudioLight->rimColor[2],0.06f*m);
            glBegin(GL_LINES);glVertex3f(rx,ry,rz);glVertex3f(0,0,0);glEnd();
            // Origin crosshair
            float c=R*0.02f;
            glColor4f(0.6f,0.6f,0.6f,0.2f);
            glBegin(GL_LINES);glVertex3f(-c,0,0);glVertex3f(c,0,0);glVertex3f(0,-c,0);glVertex3f(0,c,0);glVertex3f(0,0,-c);glVertex3f(0,0,c);glEnd();
        }
        if (_cachedStudioLight->showCones) {
            float ca = (12.f+float(_cachedStudioLight->shadowSoftness)*15.f)*kPi/180.f;
            auto thinCone=[&](float ox,float oy,float oz,float cr,float cg,float cb,float al){
                float dx=-ox,dy=-oy,dz=-oz;
                float len=std::sqrt(dx*dx+dy*dy+dz*dz); if(len<1e-4f)return;
                dx/=len;dy/=len;dz/=len;
                float cl=len*0.3f,br=cl*std::tan(ca);
                float bx=ox+dx*cl,by=oy+dy*cl,bz=oz+dz*cl;
                float px,py,pz;
                if(std::abs(dy)<0.9f){px=-dz;py=0;pz=dx;}else{px=1;py=0;pz=0;}
                float pl=std::sqrt(px*px+py*py+pz*pz);if(pl>1e-6f){px/=pl;py/=pl;pz/=pl;}
                float qx=dy*pz-dz*py,qy=dz*px-dx*pz,qz=dx*py-dy*px;
                glColor4f(cr,cg,cb,al); glLineWidth(1.f);
                for(int i=0;i<6;++i){float a=float(i)/6.f*2.f*kPi;
                    glBegin(GL_LINES);glVertex3f(ox,oy,oz);
                    glVertex3f(bx+br*(px*std::cos(a)+qx*std::sin(a)),by+br*(py*std::cos(a)+qy*std::sin(a)),bz+br*(pz*std::cos(a)+qz*std::sin(a)));
                    glEnd();}
            };
            thinCone(kx,ky,kz,_cachedStudioLight->keyColor[0],_cachedStudioLight->keyColor[1],_cachedStudioLight->keyColor[2],0.06f*m);
            thinCone(fx,fy,fz,_cachedStudioLight->fillColor[0],_cachedStudioLight->fillColor[1],_cachedStudioLight->fillColor[2],0.03f*m);
            thinCone(rx,ry,rz,_cachedStudioLight->rimColor[0],_cachedStudioLight->rimColor[1],_cachedStudioLight->rimColor[2],0.03f*m);
        }
    }

    // ─── Geometry shading (loaded meshes from USD stage) ──────────────────
    if (_scene && _scene->TotalTriangles() > 0) {
        _InitGLVolShader();

        // Ensure volume texture is uploaded for shadow casting (even if shaded vol preview is off)
        if (!_volumes.empty() && _volumes[0] && _volumes[0]->IsValid() && !_glVolDensityTex) {
            _UploadGLVolTex(_volumes[0].get());
        }

        if (_glGeoProg) {
            glPushAttrib(GL_ALL_ATTRIB_BITS);
            GLint prevProg; glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);

            glEnable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glUseProgram(_glGeoProg);

            // Light uniforms (reuse viewport light computation from _DrawVolumeShaded context)
            float gLightDirX = 0.f, gLightDirY = 1.f, gLightDirZ = 0.f;
            float gLightR = 0.f, gLightG = 0.f, gLightB = 0.f;
            float gAmbR = 0.f, gAmbG = 0.f, gAmbB = 0.f;
            float vpShadowSoft = 0.f;
            bool gHasLight = _cachedEnvLight || _useBuiltinLight;
            if (gHasLight) {
                double sunElev = _cachedEnvLight ? _cachedEnvLight->sunElevation : _sunElevation;
                double sunAz = _cachedEnvLight ? _cachedEnvLight->sunAzimuth : _sunAzimuth;
                double sunInt = _cachedEnvLight ? _cachedEnvLight->sunIntensity : _sunIntensity;
                double skyInt = _cachedEnvLight ? _cachedEnvLight->skyIntensity : _skyIntensity;
                double turbidity = _turbidity;
                vpShadowSoft = float(_cachedEnvLight ? _cachedEnvLight->sunShadowSoftness : _shadowSoftness);
                double er = sunElev * M_PI / 180.0, ar = sunAz * M_PI / 180.0;
                gLightDirX = float(-std::cos(er) * std::sin(ar));
                gLightDirY = float(std::sin(er));
                gLightDirZ = float(std::cos(er) * std::cos(ar));
                float len = std::sqrt(gLightDirX*gLightDirX + gLightDirY*gLightDirY + gLightDirZ*gLightDirZ);
                if (len > 1e-6f) { gLightDirX /= len; gLightDirY /= len; gLightDirZ /= len; }
                double r, g, b;
                sunColorFromElevation(sunElev, turbidity, r, g, b);
                double elevFactor = std::max(0.1, std::min(sunElev / 12.0, 1.0));
                float si = float(sunInt * sunInt * elevFactor * 0.32);
                gLightR = float(r * si); gLightG = float(g * si); gLightB = float(b * si);
                skyColorFromElevation(sunElev, turbidity, r, g, b);
                float skyMul = float(std::min(skyInt * 0.3, 2.0));
                gAmbR = float(r * skyMul); gAmbG = float(g * skyMul); gAmbB = float(b * skyMul);
            }

            glUniform3f(glGetUniformLocation(_glGeoProg, "uLightDir"), gLightDirX, gLightDirY, gLightDirZ);
            glUniform3f(glGetUniformLocation(_glGeoProg, "uLightCol"), gLightR, gLightG, gLightB);
            glUniform3f(glGetUniformLocation(_glGeoProg, "uAmbient"), gAmbR, gAmbG, gAmbB);

            // Constant mode: scanlineCompat + no lights = flat colour in viewport
            {
                bool noLights = (gLightR < 0.001f && gLightG < 0.001f && gLightB < 0.001f &&
                                 gAmbR < 0.001f && gAmbG < 0.001f && gAmbB < 0.001f);
                glUniform1i(glGetUniformLocation(_glGeoProg, "uConstantMode"),
                            (_scanlineCompat && noLights) ? 1 : 0);
            }

            // ─── Shadow map pass: render geometry from light POV ───
            float lightVP[16] = {};
            bool hasShadowMap = false;
            if (_glShadowFBO && _glShadowDepthProg && _glShadowDepthTex && gHasLight) {
                // Compute scene bounds from all meshes
                float scnMin[3] = {1e30f, 1e30f, 1e30f};
                float scnMax[3] = {-1e30f, -1e30f, -1e30f};
                for (const auto& kv : _scene->GetMeshes()) {
                    for (const auto& tri : kv.second.triangles) {
                        for (int a = 0; a < 3; a++) {
                            scnMin[a] = std::min({scnMin[a], tri.v0[a], tri.v1[a], tri.v2[a]});
                            scnMax[a] = std::max({scnMax[a], tri.v0[a], tri.v1[a], tri.v2[a]});
                        }
                    }
                }
                // Include volume bbox
                if (!_volumes.empty() && _volumes[0]) {
                    auto vMin = _volumes[0]->GetBboxMin();
                    auto vMax = _volumes[0]->GetBboxMax();
                    for (int a = 0; a < 3; a++) {
                        scnMin[a] = std::min(scnMin[a], vMin[a]);
                        scnMax[a] = std::max(scnMax[a], vMax[a]);
                    }
                }
                float cx = (scnMin[0]+scnMax[0])*0.5f, cy = (scnMin[1]+scnMax[1])*0.5f, cz = (scnMin[2]+scnMax[2])*0.5f;
                float extent = 0.f;
                for (int a = 0; a < 3; a++) extent = std::max(extent, scnMax[a] - scnMin[a]);
                extent *= 0.6f;

                // Light view: look from far away along light direction toward scene center
                float lx = cx + gLightDirX * extent * 2.f;
                float ly = cy + gLightDirY * extent * 2.f;
                float lz = cz + gLightDirZ * extent * 2.f;

                // Build light view matrix (lookAt)
                float fwd[3] = {cx-lx, cy-ly, cz-lz};
                float fLen = std::sqrt(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
                if (fLen > 1e-6f) { fwd[0]/=fLen; fwd[1]/=fLen; fwd[2]/=fLen; }
                float up[3] = {0,1,0};
                if (std::abs(fwd[1]) > 0.99f) { up[0]=1; up[1]=0; up[2]=0; }
                float right[3] = {fwd[1]*up[2]-fwd[2]*up[1], fwd[2]*up[0]-fwd[0]*up[2], fwd[0]*up[1]-fwd[1]*up[0]};
                float rLen = std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
                if (rLen > 1e-6f) { right[0]/=rLen; right[1]/=rLen; right[2]/=rLen; }
                float up2[3] = {right[1]*fwd[2]-right[2]*fwd[1], right[2]*fwd[0]-right[0]*fwd[2], right[0]*fwd[1]-right[1]*fwd[0]};

                // View matrix (column-major for GL)
                float view[16] = {
                    right[0], up2[0], -fwd[0], 0,
                    right[1], up2[1], -fwd[1], 0,
                    right[2], up2[2], -fwd[2], 0,
                    -(right[0]*lx+right[1]*ly+right[2]*lz),
                    -(up2[0]*lx+up2[1]*ly+up2[2]*lz),
                    (fwd[0]*lx+fwd[1]*ly+fwd[2]*lz), 1
                };

                // Ortho projection (column-major)
                float zNear = 0.1f, zFar = extent * 5.f;
                float proj[16] = {
                    1.f/extent, 0, 0, 0,
                    0, 1.f/extent, 0, 0,
                    0, 0, -2.f/(zFar-zNear), 0,
                    0, 0, -(zFar+zNear)/(zFar-zNear), 1
                };

                // lightVP = proj * view (column-major multiply)
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        lightVP[c*4+r] = 0;
                        for (int k = 0; k < 4; k++)
                            lightVP[c*4+r] += proj[k*4+r] * view[c*4+k];
                    }

                // Render shadow map
                GLint prevFBO; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
                GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);

                glBindFramebuffer(GL_FRAMEBUFFER, _glShadowFBO);
                glViewport(0, 0, kShadowMapSize, kShadowMapSize);
                glClear(GL_DEPTH_BUFFER_BIT);
                glEnable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

                glUseProgram(_glShadowDepthProg);
                glUniformMatrix4fv(glGetUniformLocation(_glShadowDepthProg, "uLightVP"), 1, GL_FALSE, lightVP);

                for (const auto& kv : _scene->GetMeshes()) {
                    if (!kv.second.visible) continue;
                    glBegin(GL_TRIANGLES);
                    for (const auto& tri : kv.second.triangles) {
                        glVertex3f(tri.v0[0], tri.v0[1], tri.v0[2]);
                        glVertex3f(tri.v1[0], tri.v1[1], tri.v1[2]);
                        glVertex3f(tri.v2[0], tri.v2[1], tri.v2[2]);
                    }
                    glEnd();
                }

                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
                glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
                hasShadowMap = true;
            }

            // Re-bind geo shader after shadow pass
            glUseProgram(_glGeoProg);
            glUniform3f(glGetUniformLocation(_glGeoProg, "uLightDir"), gLightDirX, gLightDirY, gLightDirZ);
            glUniform3f(glGetUniformLocation(_glGeoProg, "uLightCol"), gLightR, gLightG, gLightB);
            glUniform3f(glGetUniformLocation(_glGeoProg, "uAmbient"), gAmbR, gAmbG, gAmbB);
            {
                bool noLights = (gLightR < 0.001f && gLightG < 0.001f && gLightB < 0.001f &&
                                 gAmbR < 0.001f && gAmbG < 0.001f && gAmbB < 0.001f);
                glUniform1i(glGetUniformLocation(_glGeoProg, "uConstantMode"),
                            (_scanlineCompat && noLights) ? 1 : 0);
            }
            glUniform1f(glGetUniformLocation(_glGeoProg, "uHasShadowMap"), hasShadowMap ? 1.f : 0.f);
            glUniform1f(glGetUniformLocation(_glGeoProg, "uShadowTexelSize"), 1.f / float(kShadowMapSize));
            glUniform1f(glGetUniformLocation(_glGeoProg, "uShadowSoftness"), vpShadowSoft);
            glUniform1i(glGetUniformLocation(_glGeoProg, "uShadowPCFRadius"),
                        std::min(std::max(_vpShadowPCF, 0), 3));
            static const int volShadLUT[] = {0, 4, 8, 16};
            glUniform1i(glGetUniformLocation(_glGeoProg, "uVolShadowSamples"),
                        volShadLUT[std::min(std::max(_vpVolShadowSamples, 0), 3)]);
            if (hasShadowMap) {
                glUniformMatrix4fv(glGetUniformLocation(_glGeoProg, "uLightVP"), 1, GL_FALSE, lightVP);
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, _glShadowDepthTex);
                glUniform1i(glGetUniformLocation(_glGeoProg, "uShadowMap"), 2);
            }

            // Volume shadow texture (if available)
            bool hasVolTex = _glVolDensityTex && !_volumes.empty();
            glUniform1f(glGetUniformLocation(_glGeoProg, "uHasVolume"), hasVolTex ? 1.f : 0.f);
            if (hasVolTex) {
                auto& vol = _volumes[0];
                pxr::GfVec3f bMin = vol->GetBboxMin(), bMax = vol->GetBboxMax();
                glUniform3f(glGetUniformLocation(_glGeoProg, "uBboxMin"), bMin[0], bMin[1], bMin[2]);
                glUniform3f(glGetUniformLocation(_glGeoProg, "uBboxMax"), bMax[0], bMax[1], bMax[2]);
                glUniform1f(glGetUniformLocation(_glGeoProg, "uDensityMult"), 1.f / _glVolMaxDensity);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_3D, _glVolDensityTex);
                glUniform1i(glGetUniformLocation(_glGeoProg, "uDensity"), 0);
                // Volume transform for shadow UVW
                glUniform1f(glGetUniformLocation(_glGeoProg, "uHasVolXform"), vol->hasTransform ? 1.f : 0.f);
                if (vol->hasTransform) {
                    glUniform3f(glGetUniformLocation(_glGeoProg, "uVolCenter"), vol->_center[0], vol->_center[1], vol->_center[2]);
                    glUniform3f(glGetUniformLocation(_glGeoProg, "uVolInvScale"), vol->_invScale[0], vol->_invScale[1], vol->_invScale[2]);
                    glUniform3f(glGetUniformLocation(_glGeoProg, "uVolOrigMin"), vol->bboxMin[0], vol->bboxMin[1], vol->bboxMin[2]);
                    glUniform3f(glGetUniformLocation(_glGeoProg, "uVolOrigMax"), vol->bboxMax[0], vol->bboxMax[1], vol->bboxMax[2]);
                    glUniformMatrix3fv(glGetUniformLocation(_glGeoProg, "uVolInvRotM"), 1, GL_FALSE, vol->_rotM);
                }
            }

            // Environment reflection uniform
            glUniform1f(glGetUniformLocation(_glGeoProg, "uHasEnvRefl"), _vpEnvReflections ? 1.f : 0.f);

            // ─── Geometry reflection pre-pass (SSR with R32F depth) ───
            bool hasGeoRefl = false;
            if (_vpGeoReflections && _glGeoProg) {
                GLint vpDims[4]; glGetIntegerv(GL_VIEWPORT, vpDims);
                int rW = vpDims[2] / 2, rH = vpDims[3] / 2;
                if (rW < 64) rW = 64; if (rH < 64) rH = 64;

                // Recreate FBO if size changed
                if (_glReflFBO && (_glReflW != rW || _glReflH != rH)) {
                    glDeleteFramebuffers(1, &_glReflFBO); _glReflFBO = 0;
                    glDeleteTextures(1, &_glReflColorTex); _glReflColorTex = 0;
                    glDeleteTextures(1, &_glReflDepthTex); _glReflDepthTex = 0;
                }
                if (!_glReflFBO) {
                    // Color texture
                    glGenTextures(1, &_glReflColorTex);
                    glBindTexture(GL_TEXTURE_2D, _glReflColorTex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rW, rH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    // Depth texture (GL depth — both geo and volume write to this naturally)
                    glGenTextures(1, &_glReflDepthTex);
                    glBindTexture(GL_TEXTURE_2D, _glReflDepthTex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, rW, rH, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
                    // FBO: color + depth texture
                    glGenFramebuffers(1, &_glReflFBO);
                    glBindFramebuffer(GL_FRAMEBUFFER, _glReflFBO);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _glReflColorTex, 0);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _glReflDepthTex, 0);
                    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                        glDeleteFramebuffers(1, &_glReflFBO); _glReflFBO = 0;
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    _glReflW = rW; _glReflH = rH;
                }

                if (_glReflFBO) {
                    // Build normal MVP
                    float mvMat[16], projMat[16], mvpMat[16];
                    glGetFloatv(GL_MODELVIEW_MATRIX, mvMat);
                    glGetFloatv(GL_PROJECTION_MATRIX, projMat);
                    for (int r2=0;r2<4;r2++) for (int c2=0;c2<4;c2++) {
                        mvpMat[c2*4+r2]=0;
                        for (int k2=0;k2<4;k2++) mvpMat[c2*4+r2]+=projMat[k2*4+r2]*mvMat[c2*4+k2];
                    }

                    // Build reflected MV: negate camera-space Y (row 1 in column-major)
                    // This mirrors the camera view vertically — objects above appear below
                    float reflMV[16], reflMVP[16];
                    for (int i=0;i<16;i++) reflMV[i]=mvMat[i];
                    reflMV[1]=-reflMV[1]; reflMV[5]=-reflMV[5];
                    reflMV[9]=-reflMV[9]; reflMV[13]=-reflMV[13];
                    for (int r2=0;r2<4;r2++) for (int c2=0;c2<4;c2++) {
                        reflMVP[c2*4+r2]=0;
                        for (int k2=0;k2<4;k2++) reflMVP[c2*4+r2]+=projMat[k2*4+r2]*reflMV[c2*4+k2];
                    }

                    // Set GL to reflected camera for pre-pass rendering
                    glMatrixMode(GL_MODELVIEW);
                    glPushMatrix();
                    glLoadMatrixf(reflMV);
                    glFrontFace(GL_CW);  // handedness flipped

                    GLint prevFBO2; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO2);
                    glBindFramebuffer(GL_FRAMEBUFFER, _glReflFBO);
                    glViewport(0, 0, _glReflW, _glReflH);
                    glClearColor(0,0,0,0);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    glEnable(GL_DEPTH_TEST);

                    // ── Draw volume to reflection FBO ──
                    if (_vpVolReflections && _vdbShadedPreview && _glVolProg && !_volumes.empty() && _volumes[0] && _volumes[0]->IsValid()) {
                        bool vpHasAnyLight2 = _cachedEnvLight || _useBuiltinLight;
                        if (vpHasAnyLight2) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            _DrawVolumeShaded(nullptr, true);
                            glDisable(GL_BLEND);
                        }
                    }

                    // ── Draw geometry to reflection FBO ──
                    glDisable(GL_BLEND);
                    glUseProgram(_glGeoProg);
                    glUniform1f(glGetUniformLocation(_glGeoProg, "uHasGeoRefl"), 0.f);
                    glUniformMatrix4fv(glGetUniformLocation(_glGeoProg, "uMVP"), 1, GL_FALSE, reflMVP);

                    for (const auto& kv2 : _scene->GetMeshes()) {
                        if (!kv2.second.visible) continue;
                        float pmr=0.7f,pmg=0.7f,pmb=0.7f,pmet=0.f,prou=0.5f;
                        if (!kv2.second.triangles.empty()) {
                            const auto& pm=_scene->GetMaterial(kv2.second.triangles[0].materialId);
                            pmr=pm.baseColor[0];pmg=pm.baseColor[1];pmb=pm.baseColor[2];
                            pmet=pm.metallic;prou=pm.roughness;
                        }
                        glUniform3f(glGetUniformLocation(_glGeoProg,"uMatColor"),pmr,pmg,pmb);
                        glUniform1f(glGetUniformLocation(_glGeoProg,"uMetallic"),pmet);
                        glUniform1f(glGetUniformLocation(_glGeoProg,"uRoughness"),prou);
                        glBegin(GL_TRIANGLES);
                        for (const auto& tri : kv2.second.triangles) {
                            if (_scanlineCompat) {
                                glNormal3f(tri.n0[0],tri.n0[1],tri.n0[2]);
                                glVertex3f(tri.v0[0],tri.v0[1],tri.v0[2]);
                                glNormal3f(tri.n1[0],tri.n1[1],tri.n1[2]);
                                glVertex3f(tri.v1[0],tri.v1[1],tri.v1[2]);
                                glNormal3f(tri.n2[0],tri.n2[1],tri.n2[2]);
                                glVertex3f(tri.v2[0],tri.v2[1],tri.v2[2]);
                            } else {
                                pxr::GfVec3f e1=tri.v1-tri.v0,e2=tri.v2-tri.v0;
                                pxr::GfVec3f fn(e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]);
                                glNormal3f(fn[0],fn[1],fn[2]);
                                glVertex3f(tri.v0[0],tri.v0[1],tri.v0[2]);
                                glVertex3f(tri.v1[0],tri.v1[1],tri.v1[2]);
                                glVertex3f(tri.v2[0],tri.v2[1],tri.v2[2]);
                            }
                        }
                        glEnd();
                    }

                    // Restore GL state
                    glFrontFace(GL_CCW);
                    glMatrixMode(GL_MODELVIEW);
                    glPopMatrix();

                    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO2);
                    glViewport(vpDims[0],vpDims[1],vpDims[2],vpDims[3]);
                    glEnable(GL_BLEND);

                    // Main pass uniforms — pass both normal and reflected MVP
                    glUniform1f(glGetUniformLocation(_glGeoProg,"uHasGeoRefl"),1.f);
                    glUniformMatrix4fv(glGetUniformLocation(_glGeoProg,"uMVP"),1,GL_FALSE,mvpMat);
                    glUniformMatrix4fv(glGetUniformLocation(_glGeoProg,"uReflMVP"),1,GL_FALSE,reflMVP);
                    glActiveTexture(GL_TEXTURE3);
                    glBindTexture(GL_TEXTURE_2D, _glReflColorTex);
                    glUniform1i(glGetUniformLocation(_glGeoProg,"uReflColorTex"),3);
                    glActiveTexture(GL_TEXTURE4);
                    glBindTexture(GL_TEXTURE_2D, _glReflDepthTex);
                    glUniform1i(glGetUniformLocation(_glGeoProg,"uReflDepthTex"),4);
                    hasGeoRefl = true;
                }
            }
            if (!hasGeoRefl) {
                glUniform1f(glGetUniformLocation(_glGeoProg,"uHasGeoRefl"),0.f);
            }

            // Re-activate geo shader and re-bind textures (pre-pass may have changed state)
            glUseProgram(_glGeoProg);

            // Re-bind volume texture for shadow casting
            if (hasVolTex) {
                auto& vol = _volumes[0];
                pxr::GfVec3f bMin = vol->GetBboxMin(), bMax = vol->GetBboxMax();
                glUniform3f(glGetUniformLocation(_glGeoProg, "uBboxMin"), bMin[0], bMin[1], bMin[2]);
                glUniform3f(glGetUniformLocation(_glGeoProg, "uBboxMax"), bMax[0], bMax[1], bMax[2]);
                glUniform1f(glGetUniformLocation(_glGeoProg, "uDensityMult"), 1.f / _glVolMaxDensity);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_3D, _glVolDensityTex);
                glUniform1i(glGetUniformLocation(_glGeoProg, "uDensity"), 0);
                glUniform1f(glGetUniformLocation(_glGeoProg, "uHasVolXform"), vol->hasTransform ? 1.f : 0.f);
                if (vol->hasTransform) {
                    glUniform3f(glGetUniformLocation(_glGeoProg, "uVolCenter"), vol->_center[0], vol->_center[1], vol->_center[2]);
                    glUniform3f(glGetUniformLocation(_glGeoProg, "uVolInvScale"), vol->_invScale[0], vol->_invScale[1], vol->_invScale[2]);
                    glUniform3f(glGetUniformLocation(_glGeoProg, "uVolOrigMin"), vol->bboxMin[0], vol->bboxMin[1], vol->bboxMin[2]);
                    glUniform3f(glGetUniformLocation(_glGeoProg, "uVolOrigMax"), vol->bboxMax[0], vol->bboxMax[1], vol->bboxMax[2]);
                    glUniformMatrix3fv(glGetUniformLocation(_glGeoProg, "uVolInvRotM"), 1, GL_FALSE, vol->_rotM);
                }
            }
            // Re-bind shadow map (pre-pass may have changed texture state)
            if (hasShadowMap) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, _glShadowDepthTex);
                glUniform1i(glGetUniformLocation(_glGeoProg, "uShadowMap"), 2);
            }

            for (const auto& kv : _scene->GetMeshes()) {
                if (!kv.second.visible) continue;
                // Get material properties
                float mr = 0.7f, mg = 0.7f, mb = 0.7f;
                float metallic = 0.f, roughness = 0.5f;
                int baseColorTexId = -1;
                if (!kv.second.triangles.empty()) {
                    int matId = kv.second.triangles[0].materialId;
                    const auto& mat = _scene->GetMaterial(matId);
                    mr = mat.baseColor[0]; mg = mat.baseColor[1]; mb = mat.baseColor[2];
                    metallic = mat.metallic;
                    roughness = mat.roughness;
                    baseColorTexId = mat.baseColorTexId;
                }
                glUniform3f(glGetUniformLocation(_glGeoProg, "uMatColor"), mr, mg, mb);
                glUniform1f(glGetUniformLocation(_glGeoProg, "uMetallic"), metallic);
                glUniform1f(glGetUniformLocation(_glGeoProg, "uRoughness"), roughness);

                // Upload/bind base color texture for viewport
                bool hasVpTex = false;
                if (baseColorTexId >= 0 && _scene) {
                    const auto* tex = _scene->GetTexture(baseColorTexId);
                    if (tex && tex->IsValid() && tex->_width > 0 && tex->_height > 0) {
                        if (!_glBaseColorTex) glGenTextures(1, &_glBaseColorTex);
                        glActiveTexture(GL_TEXTURE5);
                        glBindTexture(GL_TEXTURE_2D, _glBaseColorTex);
                        // Upload RGBA (or RGB padded to RGBA)
                        if (tex->_channels >= 4) {
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->_width, tex->_height,
                                         0, GL_RGBA, GL_FLOAT, tex->_pixels.data());
                        } else {
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tex->_width, tex->_height,
                                         0, GL_RGB, GL_FLOAT, tex->_pixels.data());
                        }
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                        glUniform1i(glGetUniformLocation(_glGeoProg, "uBaseColorTex"), 5);
                        hasVpTex = true;
                    }
                }
                glUniform1i(glGetUniformLocation(_glGeoProg, "uHasBaseColorTex"), hasVpTex ? 1 : 0);

                glBegin(GL_TRIANGLES);
                for (const auto& tri : kv.second.triangles) {
                    if (_scanlineCompat) {
                        // Smooth vertex normals + UVs
                        glTexCoord2f(tri.uv0[0], 1.f - tri.uv0[1]);
                        glNormal3f(tri.n0[0], tri.n0[1], tri.n0[2]);
                        glVertex3f(tri.v0[0], tri.v0[1], tri.v0[2]);
                        glTexCoord2f(tri.uv1[0], 1.f - tri.uv1[1]);
                        glNormal3f(tri.n1[0], tri.n1[1], tri.n1[2]);
                        glVertex3f(tri.v1[0], tri.v1[1], tri.v1[2]);
                        glTexCoord2f(tri.uv2[0], 1.f - tri.uv2[1]);
                        glNormal3f(tri.n2[0], tri.n2[1], tri.n2[2]);
                        glVertex3f(tri.v2[0], tri.v2[1], tri.v2[2]);
                    } else {
                        // Face normal for flat shading
                        pxr::GfVec3f e1 = tri.v1 - tri.v0, e2 = tri.v2 - tri.v0;
                        pxr::GfVec3f fn(e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]);
                        glNormal3f(fn[0], fn[1], fn[2]);
                        glVertex3f(tri.v0[0], tri.v0[1], tri.v0[2]);
                        glVertex3f(tri.v1[0], tri.v1[1], tri.v1[2]);
                        glVertex3f(tri.v2[0], tri.v2[1], tri.v2[2]);
                    }
                }
                glEnd();
            }

            if (hasVolTex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_3D, 0); }
            if (hasShadowMap) { glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0); }
            if (_glBaseColorTex) { glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, 0); }
            if (hasGeoRefl) {
                glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, 0);
            }
            glActiveTexture(GL_TEXTURE0);
            glUseProgram(prevProg);
            glPopAttrib();
        }
    }

    // ─── Shaded volume (drawn AFTER geometry so depth test clips correctly) ───
    {
        bool vpHasAnyLight = _cachedEnvLight || _useBuiltinLight;
        if (_vdbShadedPreview && !_volumes.empty() && _volumes[0] && _volumes[0]->IsValid() && vpHasAnyLight) {
            _DrawVolumeShaded(ctx);
        }
    }

    glPopAttrib();
}

// ---------------------------------------------------------------------------
// _LoadVDB — load OpenVDB file and resample to uniform grid
// ---------------------------------------------------------------------------
void SpectralRenderIop::_LoadVDB()
{
    // Skip if already loaded for this frame
    int curFrame = int(outputContext().frame());
    if (_volume && _volume->IsValid() && _vdbLastLoadedFrame == curFrame) {
        // Only apply local shading for single-VDB path (not VolMerge)
        if (_volumes.empty()) {
            _applyVolumeShading(_volume);
        }
        return;
    }
    _vdbLastLoadedFrame = curFrame;

    // Path 1: File knob on SpectralRender
#ifdef SPECTRAL_HAS_VDB
    if (_vdbFile && strlen(_vdbFile) > 0) {
        int frame = int(outputContext().frame()) + _vdbFrameOffset;
        std::string resolvedPath = _vdbAutoSequence
            ? _resolveFramePath(frame)
            : std::string(_vdbFile);
        if (!resolvedPath.empty()) {
            if (!_volume || !_volume->IsValid() || _vdbLoadedPath != resolvedPath) {
                // Check LRU cache first — instant if recently visited
                VDBCacheEntry* cached = _VDBCacheGet(resolvedPath);
                if (cached) {
                    _volume = cached->volume;
                    _vdbIsPreviewRes = cached->isPreviewRes;
                    _vdbIsMetadataOnly = cached->isMetadataOnly;
                    _vdbLoadedPath = resolvedPath;
                    _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
                } else if (_vdbFastScrub) {
                    // Metadata only — just bbox, ~1ms
                    _volume = pxr::SpectralVDBLoader::LoadMetadataOnly(resolvedPath.c_str(),
                        _VdbGridName(_vdbDensityGridIdx, _vdbDensityOverride));
                    _vdbIsMetadataOnly = true;
                    _vdbIsPreviewRes = true;
                    if (_volume) {
                        _vdbLoadedPath = resolvedPath;
                        _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
                        _VDBCachePut(resolvedPath, _volume, true, true);
                    }
                } else {
                    // Full load at viewport res (128³)
                    _volume = pxr::SpectralVDBLoader::Load(resolvedPath.c_str(),
                        _VdbGridName(_vdbDensityGridIdx, _vdbDensityOverride),
                        _VdbGridName(_vdbTempGridIdx, _vdbTempOverride),
                        128);
                    _vdbIsMetadataOnly = false;
                    _vdbIsPreviewRes = true;
                    if (_volume) {
                        _vdbLoadedPath = resolvedPath;
                        _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
                        _VDBCachePut(resolvedPath, _volume, true, false);
                    }
                }
            }
            return;
        }
    }
#endif

    // Path 2: Scn input (input 1) — find ALL SpectralVDBRead upstream through GeoScene
    if (inputs() > 1 && input(1)) {
        Op* scn = input(1);
        scn->validate(true);

        // Search scn and its inputs for SpectralVolMerge, VDBRead, and light nodes
        std::vector<SpectralVDBRead*> vdbReads;
        SpectralVolMerge* volMerge = nullptr;
        _cachedEnvLight = nullptr;
        _cachedStudioLight = nullptr;
        _allEnvLights.clear();
        _allStudioLights.clear();
        std::vector<Op*> searchOps;
        searchOps.push_back(scn);
        for (int depth = 0; depth < 4; ++depth) {
            std::vector<Op*> nextLevel;
            for (Op* op : searchOps) {
                if (!op) continue;
                if (strcmp(op->Class(), "SpectralVDBRead") == 0 && !op->node_disabled())
                    vdbReads.push_back(static_cast<SpectralVDBRead*>(op));
                if (!volMerge && strcmp(op->Class(), "SpectralVolMerge") == 0 && !op->node_disabled())
                    volMerge = static_cast<SpectralVolMerge*>(op);
                if (strcmp(op->Class(), "SpectralEnvLight") == 0 && !op->node_disabled()) {
                    auto* el = static_cast<SpectralEnvLight*>(op);
                    _allEnvLights.push_back(el);
                    if (!_cachedEnvLight) {
                        _cachedEnvLight = el;
                        // Copy volume env params immediately so _applyVolumeShading works
                        _vdbEnvIntensity = el->envIntensity;
                        _vdbEnvDiffuse = el->envDiffuse;
                        _vdbEnvMode = el->envMode;
                        _vdbEnvVirtualLights = el->envVirtualLights;
                        _vdbUseReSTIR = el->useReSTIR;
                    }
                }
                if (strcmp(op->Class(), "SpectralStudioLight") == 0 && !op->node_disabled()) {
                    auto* sl = static_cast<SpectralStudioLight*>(op);
                    _allStudioLights.push_back(sl);
                    if (!_cachedStudioLight) _cachedStudioLight = sl;
                }
                for (int i = 0; i < op->inputs(); ++i) {
                    Op* up = op->input(i);
                    if (up) {
                        up->validate(true);
                        nextLevel.push_back(up);
                    }
                }
            }
            searchOps = nextLevel;
        }

        // Prefer VolMerge for multi-volume (handles transforms properly)
        if (volMerge) {
            _cachedVolMerge = volMerge;
            // Preview res for viewport — full res loaded later in _EnsureFrameRendered
            static const int vpResLUT[] = {32, 64, 128, 256};
            int previewMaxRes = vpResLUT[std::min(std::max(_vdbViewportRes, 0), 3)];
            auto entries = volMerge->GetVolumes(int(outputContext().frame()), previewMaxRes);

            // Collect SpectralVolumeMaterial ops from VolMerge inputs (in order)
            // Materials are mapped positionally to volume entries:
            // 1st material → 1st volume, 2nd material → 2nd volume, etc.
            std::vector<SpectralVolumeMaterial*> volMats;
            for (int inp = 0; inp < volMerge->inputs(); ++inp) {
                Op* up = volMerge->input(inp);
                if (!up || up->node_disabled()) continue;
                up->validate(true);
                // Check this input and walk its chain for a VolumeMaterial
                Op* cur = up;
                for (int d = 0; d < 6 && cur; ++d) {
                    if (strcmp(cur->Class(), "SpectralVolumeMaterial") == 0) {
                        volMats.push_back(static_cast<SpectralVolumeMaterial*>(cur));
                        break;
                    }
                    cur = (cur->inputs() > 0) ? cur->input(0) : nullptr;
                }
            }

            _volumes.clear();
            for (auto& e : entries) {
                if (e.volume && e.volume->IsValid()) {
                    // Find material from VDBRead's mat input (works across chained VolMerges)
                    bool appliedMat = false;
                    if (e.vdbRead && e.vdbRead->inputs() > 0 && e.vdbRead->input(0)) {
                        Op* matOp = e.vdbRead->input(0);
                        if (matOp) matOp->validate(true);
                        // Walk up to 4 levels to find SpectralVolumeMaterial
                        Op* cur = matOp;
                        for (int d = 0; d < 4 && cur; ++d) {
                            if (strcmp(cur->Class(), "SpectralVolumeMaterial") == 0) {
                                _applyVolumeMaterialDirect(e.volume,
                                    static_cast<SpectralVolumeMaterial*>(cur));
                                appliedMat = true;
                                break;
                            }
                            cur = (cur->inputs() > 0) ? cur->input(0) : nullptr;
                        }
                    }
                    if (!appliedMat) {
                        // Fallback: try positional from top-level VolMerge inputs
                        int volIdx = (int)_volumes.size();
                        if (volIdx < (int)volMats.size()) {
                            _applyVolumeMaterialDirect(e.volume, volMats[volIdx]);
                        } else if (!volMats.empty()) {
                            _applyVolumeMaterialDirect(e.volume, volMats.back());
                        } else {
                            _applyVolumeShading(e.volume);
                        }
                    }
                    _volumes.push_back(e.volume);
                }
            }
            // Get lights from VolMerge if not already found
            if (!_cachedEnvLight) _cachedEnvLight = volMerge->GetEnvLight();
            if (!_cachedStudioLight) _cachedStudioLight = volMerge->GetStudioLight();
            _volume = _volumes.empty() ? nullptr : _volumes[0];
            _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
            SLOG("SpectralRender: loaded %d volume(s) from VolMerge\n", (int)_volumes.size());
            for (size_t vi = 0; vi < _volumes.size(); ++vi) {
                auto& v = _volumes[vi];
                SLOG("  vol[%zu] scatter=(%.2f,%.2f,%.2f) ext=%.2f scat=%.2f int=%.2f envI=%.2f envD=%.2f\n",
                    vi, v->scatterColor[0], v->scatterColor[1], v->scatterColor[2],
                    v->extinction, v->scattering, v->intensity, v->envIntensity, v->envDiffuse);
            }
            return;
        }

        // Fallback: individual VDBRead nodes
        _cachedVolMerge = nullptr;
        if (!vdbReads.empty()) {
            _volumes.clear();
            _cachedVdbReads.clear();
            int renderFrame = int(outputContext().frame());
            static const int vpResLUT2[] = {32, 64, 128, 256};
            int previewMaxRes = vpResLUT2[std::min(std::max(_vdbViewportRes, 0), 3)];  // viewport res knob

            // For each VDBRead, find its GeoTransform by walking from scn input
            struct VDBWithXform { SpectralVDBRead* vdb; Op* chainTop; };
            std::vector<VDBWithXform> vdbEntries;

            // Walk from scn input itself (may be GeoTransform → VDBRead)
            Op* scnOp = input(1);
            if (scnOp) {
                // Check scnOp and its chain for VDBReads
                std::vector<Op*> toSearch;
                toSearch.push_back(scnOp);
                // Also check scnOp's inputs (for GeoScene-style multi-input)
                for (int inp = 0; inp < scnOp->inputs(); ++inp)
                    if (scnOp->input(inp)) toSearch.push_back(scnOp->input(inp));

                for (Op* startOp : toSearch) {
                    Op* cur = startOp;
                    SpectralVDBRead* vdb = nullptr;
                    for (int d = 0; d < 6 && cur; ++d) {
                        if (strcmp(cur->Class(), "SpectralVDBRead") == 0 && !cur->node_disabled()) {
                            vdb = static_cast<SpectralVDBRead*>(cur);
                            break;
                        }
                        cur = (cur->inputs() > 0) ? cur->input(0) : nullptr;
                    }
                    if (vdb) vdbEntries.push_back({vdb, startOp});
                }
            }
            // Fallback: use discovered vdbReads if chain walk found nothing
            if (vdbEntries.empty()) {
                for (auto* vdb : vdbReads) vdbEntries.push_back({vdb, nullptr});
            }

            for (size_t vi = 0; vi < vdbEntries.size(); ++vi) {
                auto& entry = vdbEntries[vi];
                entry.vdb->validate(true);
                int nodeRes = entry.vdb->GetMaxRes();
                int effectiveRes = std::min(previewMaxRes, nodeRes);
                auto vol = entry.vdb->GetVolumeAtFrame(renderFrame, effectiveRes);
                if (vol && vol->IsValid()) {
                    _applyVolumeShading(vol, entry.vdb);

                    // Read GeoTransform from the chain (walk from chainTop DOWN to VDBRead)
                    if (entry.chainTop) {
                        Op* cur = entry.chainTop;
                        for (int d = 0; d < 6 && cur; ++d) {
                            if (cur == entry.vdb) break;  // reached VDBRead, stop
                            if (strcmp(cur->Class(), "GeoTransform") == 0 ||
                                strcmp(cur->Class(), "TransformGeo") == 0) {
                                pxr::GfVec3f t(0), r(0), s(1);
                                const char* tNames[] = {"translate","xform_translate","trans","position",nullptr};
                                for (const char** tn = tNames; *tn; ++tn)
                                    if (Knob* k = cur->knob(*tn)) { t = pxr::GfVec3f(float(k->get_value(0)),float(k->get_value(1)),float(k->get_value(2))); break; }
                                const char* rNames[] = {"rotate","xform_rotate","rot",nullptr};
                                for (const char** rn = rNames; *rn; ++rn)
                                    if (Knob* k = cur->knob(*rn)) { r = pxr::GfVec3f(float(k->get_value(0)),float(k->get_value(1)),float(k->get_value(2))); break; }
                                const char* sNames[] = {"scaling","xform_scale","scale",nullptr};
                                for (const char** sn = sNames; *sn; ++sn)
                                    if (Knob* k = cur->knob(*sn)) { s = pxr::GfVec3f(float(k->get_value(0)),float(k->get_value(1)),float(k->get_value(2))); break; }
                                if (Knob* us = cur->knob("uniform_scale")) {
                                    float u = float(us->get_value(0));
                                    s = pxr::GfVec3f(s[0]*u, s[1]*u, s[2]*u);
                                }
                                vol->translate = t; vol->rotate = r; vol->scale = s;
                                vol->BuildTransform();
                                break;
                            }
                            cur = (cur->inputs() > 0) ? cur->input(0) : nullptr;
                        }
                    }

                    _volumes.push_back(vol);
                    _cachedVdbReads.push_back(entry.vdb);

                    // Bbox lock: stabilize world-space position across frames
                    // Applied AFTER GeoTransform to avoid rotation amplification
                    if (entry.vdb->GetLockBbox()) {
                        pxr::GfVec3f curCenter = (vol->GetBboxMin() + vol->GetBboxMax()) * 0.5f;
                        if (!_hasRefVolCenter) {
                            _refVolCenter = curCenter;
                            _hasRefVolCenter = true;
                        } else {
                            pxr::GfVec3f offset = _refVolCenter - curCenter;
                            if (vol->hasTransform) {
                                vol->xfBboxMin += offset;
                                vol->xfBboxMax += offset;
                                vol->_center += offset;
                            } else {
                                vol->bboxMin += offset;
                                vol->bboxMax += offset;
                            }
                        }
                    }

                    SLOG("SpectralRender: vol[%d] %dx%dx%d bbox(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                            (int)vi, vol->resX, vol->resY, vol->resZ,
                            vol->GetBboxMin()[0], vol->GetBboxMin()[1], vol->GetBboxMin()[2],
                            vol->GetBboxMax()[0], vol->GetBboxMax()[1], vol->GetBboxMax()[2]);
                }
            }
            // Primary volume for preview/backward compat
            _volume = _volumes.empty() ? nullptr : _volumes[0];
            _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
            SLOG("SpectralRender: loaded %d volume(s) from scn input chain\n",
                    (int)_volumes.size());
            for (size_t vi = 0; vi < _volumes.size(); ++vi) {
                auto& v = _volumes[vi];
                SLOG("  vol[%zu] scatter=(%.2f,%.2f,%.2f) ext=%.2f scat=%.2f int=%.2f envI=%.2f envD=%.2f\n",
                    vi, v->scatterColor[0], v->scatterColor[1], v->scatterColor[2],
                    v->extinction, v->scattering, v->intensity, v->envIntensity, v->envDiffuse);
            }
            return;
        }

        // No VDBRead or VolMerge found in scn chain — clear stale volumes
        if (_volumes.size() > 0 || _volume) {
            _volumes.clear();
            _volume.reset();
            _vdbPreviewDirty = true;
            _vdbPreviewPoints.clear();
            SLOG("SpectralRender: no volumes in scn chain — cleared\n");
        }
    }
}

// ---------------------------------------------------------------------------
// _LoadVDBForRender — reload at render resolution
// ---------------------------------------------------------------------------
void SpectralRenderIop::_LoadVDBForRender()
{
#ifdef SPECTRAL_HAS_VDB
    if ((_vdbIsPreviewRes || _vdbIsMetadataOnly) && _vdbFile && strlen(_vdbFile) > 0) {
        // Resolution modes: 0=1/8, 1=1/4, 2=1/2, 3=Full(512 cap), 4=Native
        // For fractional modes, we first need native dims from the VDB
        int renderRes;
        if (_vdbVolRes == 4) {
            renderRes = -1;  // native, capped at 1024 in loader
        } else if (_vdbVolRes == 3) {
            renderRes = 512;
        } else {
            // Get native dims to compute fraction
            int nativeDim = 256;  // fallback
            if (_volume && _volume->IsValid()) {
                // Estimate from current loaded volume
                GfVec3f bboxSize = _volume->bboxMax - _volume->bboxMin;
                float maxSize = std::max({bboxSize[0], bboxSize[1], bboxSize[2]});
                // Use DiscoverGrids to get actual voxel count
            }
            // Try to get native dims from metadata
            auto grids = pxr::SpectralVDBLoader::DiscoverGrids(_vdbFile);
            for (const auto& g : grids) {
                if (g.voxelCount > 0) {
                    // Estimate dimension from voxel count (cube root)
                    nativeDim = std::max(nativeDim, int(std::cbrt(double(g.voxelCount))));
                }
            }
            static const float fractions[] = { 0.125f, 0.25f, 0.5f };
            float frac = (_vdbVolRes >= 0 && _vdbVolRes <= 2) ? fractions[_vdbVolRes] : 0.5f;
            renderRes = std::max(32, int(nativeDim * frac));
        }

        int frame = int(outputContext().frame()) + _vdbFrameOffset;
        std::string resolvedPath = _vdbAutoSequence
            ? _resolveFramePath(frame)
            : std::string(_vdbFile);
        if (!resolvedPath.empty()) {
            std::string renderKey = resolvedPath + ":" + std::to_string(renderRes);
            VDBCacheEntry* cached = _VDBCacheGet(renderKey);
            if (cached && !cached->isPreviewRes && !cached->isMetadataOnly) {
                _volume = cached->volume;
                _vdbIsPreviewRes = false;
                _vdbIsMetadataOnly = false;
                _vdbLoadedPath = resolvedPath;
                _applyVolumeShading(_volume);
                return;
            }
            _volume = pxr::SpectralVDBLoader::Load(resolvedPath.c_str(),
                _VdbGridName(_vdbDensityGridIdx, _vdbDensityOverride),
                _VdbGridName(_vdbTempGridIdx, _vdbTempOverride),
                renderRes);
            _vdbIsPreviewRes = false;
            _vdbIsMetadataOnly = false;
            if (_volume) {
                _vdbLoadedPath = resolvedPath;
                _applyVolumeShading(_volume);
                _VDBCachePut(renderKey, _volume, false, false);
                float memMB = (_volume->density.size() + _volume->temperature.size() + _volume->flame.size())
                              * sizeof(float) / (1024.f * 1024.f);
                SLOG("SpectralRender: loaded VDB at %dx%dx%d (%.1f MB)\n",
                        _volume->resX, _volume->resY, _volume->resZ, memMB);
            }
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// _applyVolumeMaterialDirect — apply a specific SpectralVolumeMaterial to a volume
// Used by VolMerge path for per-volume material assignment.
// ---------------------------------------------------------------------------
void SpectralRenderIop::_applyVolumeMaterialDirect(
    std::shared_ptr<pxr::SpectralVolume>& vol, SpectralVolumeMaterial* mat)
{
    if (mat) {
        vol->extinction = float(mat->extinction);
        vol->scattering = float(mat->scattering);
        vol->densityMult = float(mat->densityMult);
        vol->scatterColor = pxr::GfVec3f(mat->scatterColor[0], mat->scatterColor[1], mat->scatterColor[2]);
        vol->phaseMode = mat->phaseMode;
        vol->mieDropletD = float(mat->mieDropletD);
        vol->gForward = float(mat->gForward);
        vol->gBackward = float(mat->gBackward);
        vol->lobeMix = float(mat->lobeMix);
        vol->powderStrength = float(mat->powder);
        vol->gradientMix = float(mat->gradientMix);
        vol->jitter = mat->jitter;
        vol->emissionIntensity = float(mat->emissionIntensity);
        vol->tempMin = float(mat->tempMin);
        vol->tempMax = float(mat->tempMax);
        vol->flameIntensity = float(mat->flameIntensity);
        vol->useBlackbody = mat->useBlackbody;
        vol->chromaticExtinction = mat->chromaticExtinction;
        vol->sigmaR = float(mat->sigmaR);
        vol->sigmaG = float(mat->sigmaG);
        vol->sigmaB = float(mat->sigmaB);
        vol->noiseEnable = mat->noiseEnable;
        vol->noiseNormalize = mat->noiseNormalize;
        vol->noiseScale = float(mat->noiseScale);
        vol->noiseStrength = float(mat->noiseStrength);
        vol->noiseOctaves = mat->noiseOctaves;
        vol->noiseRoughness = float(mat->noiseRoughness);
        vol->msApprox = mat->msApprox;
        vol->msTint = pxr::GfVec3f(mat->msTint[0], mat->msTint[1], mat->msTint[2]);
        // Phase 17: fire & explosions
        vol->flameOpacity = float(mat->flameOpacity);
        vol->flameTempMin = float(mat->flameTempMin);
        vol->flameTempMax = float(mat->flameTempMax);
        vol->coreGlow = float(mat->coreGlow);
        vol->coreTemp = float(mat->coreTemp);
        vol->cherenkov = mat->cherenkov;
        vol->cherenkovStrength = float(mat->cherenkovStrength);
        vol->cherenkovThreshold = float(mat->cherenkovThreshold);
        // Grid mixer
        vol->densityMix = float(mat->densityMix);
        vol->tempMix = float(mat->tempMix);
        vol->flameMix = float(mat->flameMix);
    }
    // Always from SpectralRender (rendering quality, not look)
    vol->stepSize = float(_vdbStepSize);
    vol->shadowSteps = _vdbShadowSteps;
    vol->shadowDensity = float(_vdbShadowDensity);
    vol->quality = float(_vdbQuality);
    vol->adaptiveStep = _vdbAdaptiveStep;
    vol->renderMode = _vdbRenderMode;
    vol->intensity = float(_vdbIntensity);
    vol->envIntensity = float(_vdbEnvIntensity);
    vol->envDiffuse = float(_vdbEnvDiffuse);
    vol->spectralVolumes = _vdbSpectralVolumes;
}

// ---------------------------------------------------------------------------
// _applyVolumeShading — copy shading params from knobs to volume
// If searchFrom is provided, search that Op's inputs for a SpectralVolumeMaterial.
// Otherwise, search from the scene input (input 0) for global material.
// ---------------------------------------------------------------------------
void SpectralRenderIop::_applyVolumeShading(std::shared_ptr<pxr::SpectralVolume>& vol,
                                             DD::Image::Op* searchFrom)
{
    // Find SpectralVolumeMaterial by walking input chain
    SpectralVolumeMaterial* mat = nullptr;

    Op* startOp = searchFrom ? searchFrom : (inputs() > 1 ? input(1) : nullptr);
    if (startOp) {
        std::vector<Op*> searchOps;
        searchOps.push_back(startOp);
        for (int depth = 0; depth < 4 && !mat; ++depth) {
            std::vector<Op*> nextLevel;
            for (Op* op : searchOps) {
                if (!op) continue;
                // Check if this op IS a SpectralVolumeMaterial
                if (strcmp(op->Class(), "SpectralVolumeMaterial") == 0) {
                    mat = static_cast<SpectralVolumeMaterial*>(op);
                    break;
                }
                // Check all inputs of this op for SpectralVolumeMaterial
                for (int i = 0; i < op->inputs(); ++i) {
                    Op* up = op->input(i);
                    if (!up) continue;
                    if (strcmp(up->Class(), "SpectralVolumeMaterial") == 0) {
                        mat = static_cast<SpectralVolumeMaterial*>(up);
                        break;
                    }
                    nextLevel.push_back(up);
                }
                if (mat) break;
            }
            searchOps = nextLevel;
        }
    }

    if (mat) {
        // Use SpectralVolumeMaterial parameters (shader/look)
        vol->extinction = float(mat->extinction);
        vol->scattering = float(mat->scattering);
        vol->densityMult = float(mat->densityMult);
        vol->scatterColor = pxr::GfVec3f(mat->scatterColor[0], mat->scatterColor[1], mat->scatterColor[2]);
        // Phase function
        vol->phaseMode = mat->phaseMode;
        vol->mieDropletD = float(mat->mieDropletD);
        vol->gForward = float(mat->gForward);
        vol->gBackward = float(mat->gBackward);
        vol->lobeMix = float(mat->lobeMix);
        vol->powderStrength = float(mat->powder);
        vol->gradientMix = float(mat->gradientMix);
        vol->jitter = mat->jitter;
        // Emission
        vol->emissionIntensity = float(mat->emissionIntensity);
        vol->tempMin = float(mat->tempMin);
        vol->tempMax = float(mat->tempMax);
        vol->flameIntensity = float(mat->flameIntensity);
        vol->useBlackbody = mat->useBlackbody;
        // Chromatic extinction
        vol->chromaticExtinction = mat->chromaticExtinction;
        vol->sigmaR = float(mat->sigmaR);
        vol->sigmaG = float(mat->sigmaG);
        vol->sigmaB = float(mat->sigmaB);
        // Noise
        vol->noiseEnable = mat->noiseEnable;
        vol->noiseNormalize = mat->noiseNormalize;
        vol->noiseScale = float(mat->noiseScale);
        vol->noiseStrength = float(mat->noiseStrength);
        vol->noiseOctaves = mat->noiseOctaves;
        vol->noiseRoughness = float(mat->noiseRoughness);
        // Multiple scattering
        vol->msApprox = mat->msApprox;
        vol->msTint = pxr::GfVec3f(mat->msTint[0], mat->msTint[1], mat->msTint[2]);
        // Phase 17: fire & explosions
        vol->flameOpacity = float(mat->flameOpacity);
        vol->flameTempMin = float(mat->flameTempMin);
        vol->flameTempMax = float(mat->flameTempMax);
        vol->coreGlow = float(mat->coreGlow);
        vol->coreTemp = float(mat->coreTemp);
        vol->cherenkov = mat->cherenkov;
        vol->cherenkovStrength = float(mat->cherenkovStrength);
        vol->cherenkovThreshold = float(mat->cherenkovThreshold);
        // Grid mixer
        vol->densityMix = float(mat->densityMix);
        vol->tempMix = float(mat->tempMix);
        vol->flameMix = float(mat->flameMix);
    } else {
        // Fall back to SpectralRender's own Volumes tab knobs
        vol->extinction = float(_vdbExtinction);
        vol->scattering = float(_vdbScattering);
        vol->densityMult = float(_vdbDensityMult);
        vol->anisotropy = float(_vdbAnisotropy);
        vol->emissionIntensity = float(_vdbEmissionIntensity);
        vol->tempMin = float(_vdbTempMin);
        vol->tempMax = float(_vdbTempMax);
        vol->flameIntensity = float(_vdbFlameIntensity);
        vol->gForward = float(_vdbGForward);
        vol->gBackward = float(_vdbGBackward);
        vol->lobeMix = float(_vdbLobeMix);
        vol->powderStrength = float(_vdbPowder);
        vol->jitter = _vdbJitter;
        vol->scatterColor = pxr::GfVec3f(_vdbScatterColor[0], _vdbScatterColor[1], _vdbScatterColor[2]);
        vol->useBlackbody = false;
        vol->chromaticExtinction = false;
        vol->phaseMode = _vdbPhaseMode;
        vol->mieDropletD = float(_vdbMieDropletD);
        vol->noiseEnable = _vdbNoiseEnable;
        vol->noiseNormalize = _vdbNoiseNormalize;
        vol->noiseScale = float(_vdbNoiseScale);
        vol->noiseStrength = float(_vdbNoiseStrength);
        vol->noiseOctaves = _vdbNoiseOctaves;
        vol->noiseRoughness = float(_vdbNoiseRoughness);
        vol->msApprox = _vdbMsApprox;
        vol->msTint = pxr::GfVec3f(_vdbMsTint[0], _vdbMsTint[1], _vdbMsTint[2]);
    }
    // Always from SpectralRender (rendering quality, not look)
    vol->stepSize = float(_vdbStepSize);
    vol->shadowSteps = _vdbShadowSteps;
    vol->shadowDensity = float(_vdbShadowDensity);
    vol->quality = float(_vdbQuality);
    vol->adaptiveStep = _vdbAdaptiveStep;
    vol->renderMode = _vdbRenderMode;
    vol->intensity = float(_vdbIntensity);
    vol->envIntensity = float(_vdbEnvIntensity);
    vol->envDiffuse = float(_vdbEnvDiffuse);
    vol->spectralVolumes = _vdbSpectralVolumes;

    // Volume transform (disabled — use GeoTransform upstream instead)
    // TODO: remove once GeoTransform pipeline is fully validated
    /*
    float us = float(_vdbUniformScale);
    vol->translate = pxr::GfVec3f(float(_vdbTranslate[0]), float(_vdbTranslate[1]), float(_vdbTranslate[2]));
    vol->rotate = pxr::GfVec3f(float(_vdbRotate[0]), float(_vdbRotate[1]), float(_vdbRotate[2]));
    vol->scale = pxr::GfVec3f(float(_vdbScale[0])*us, float(_vdbScale[1])*us, float(_vdbScale[2])*us);
    vol->BuildTransform();
    */
}

// ---------------------------------------------------------------------------
// VDB frame cache — LRU with 8 entries for instant scrub-back
// ---------------------------------------------------------------------------
void SpectralRenderIop::_VDBCachePut(const std::string& path,
    std::shared_ptr<pxr::SpectralVolume> vol, bool preview, bool meta)
{
    if (!_vdbCacheEnabled) return;
    _vdbCacheLRU.erase(
        std::remove(_vdbCacheLRU.begin(), _vdbCacheLRU.end(), path),
        _vdbCacheLRU.end());
    while ((int)_vdbCacheLRU.size() >= _vdbCacheMax) {
        _vdbCache.erase(_vdbCacheLRU.front());
        _vdbCacheLRU.erase(_vdbCacheLRU.begin());
    }
    _vdbCache[path] = {vol, preview, meta};
    _vdbCacheLRU.push_back(path);
}

SpectralRenderIop::VDBCacheEntry* SpectralRenderIop::_VDBCacheGet(const std::string& path)
{
    if (!_vdbCacheEnabled) return nullptr;
    auto it = _vdbCache.find(path);
    if (it == _vdbCache.end()) return nullptr;
    // Move to back of LRU
    _vdbCacheLRU.erase(
        std::remove(_vdbCacheLRU.begin(), _vdbCacheLRU.end(), path),
        _vdbCacheLRU.end());
    _vdbCacheLRU.push_back(path);
    return &it->second;
}

// ---------------------------------------------------------------------------
// _BuildLightRig — sun/sky + studio lights (Preetham model)
// ---------------------------------------------------------------------------
static void sunColorFromElevation(double elev, double turbidity, double& r, double& g, double& b) {
    // Extended model: supports negative elevation for dawn/twilight
    double t = std::max(-0.1, std::min(elev / 90.0, 1.0));
    double turbShift = (turbidity - 2.0) / 8.0 * 0.15;
    if (elev < 0) {
        // Below horizon: deep red/orange
        double f = std::max(0.0, 1.0 + elev / 10.0); // fades to 0 at -10°
        r = f; g = f * 0.2; b = f * 0.02;
    } else if (elev < 5) {
        // Near horizon: intense warm — red sky at dawn
        double f = elev / 5.0;
        r = 1.0;
        g = std::max(0.1, 0.15 + 0.35 * f - turbShift);
        b = std::max(0.01, 0.02 + 0.1 * f * f - turbShift * 2);
    } else {
        r = 1.0;
        g = std::max(0.15, std::min(0.4 + 0.55 * t - turbShift, 1.0));
        b = std::max(0.02, std::min(0.1 + 0.8 * t * t - turbShift * 2, 0.95));
    }
}

static void skyColorFromElevation(double elev, double turbidity, double& r, double& g, double& b) {
    double t = std::max(0.0, std::min(elev / 90.0, 1.0));
    double haze = std::max(0.0, std::min((turbidity - 2.0) / 8.0, 1.0));
    r = 0.15 + 0.1 * (1 - t) + 0.3 * haze;
    g = 0.3 + 0.15 * (1 - t) + 0.15 * haze;
    b = 0.7 - 0.3 * (1 - t) - 0.2 * haze;
    double grey = (r + g + b) / 3.0;
    r += (grey - r) * haze * 0.5;
    g += (grey - g) * haze * 0.5;
    b += (grey - b) * haze * 0.5;
}

static GfVec3f dirFromElevAzim(double elevDeg, double azimDeg) {
    double er = elevDeg * M_PI / 180.0, ar = azimDeg * M_PI / 180.0;
    float dx = float(std::cos(er) * std::sin(ar));
    float dy = float(-std::sin(er));
    float dz = float(-std::cos(er) * std::cos(ar));
    GfVec3f d(dx, dy, dz);
    float len = d.GetLength();
    return (len > 1e-8f) ? d / len : GfVec3f(0, -1, 0);
}

void SpectralRenderIop::_BuildLightRig()
{
    if (!_scene) return;

    // Search scn input chain for ALL SpectralEnvLight and SpectralStudioLight nodes
    // (vectors already populated in _EnsureFrameRendered scene walk)
    // Legacy fallback: direct discovery if vectors are empty
    if (_allEnvLights.empty() && _allStudioLights.empty() && inputs() > 1 && input(1)) {
        std::vector<Op*> searchOps;
        searchOps.push_back(input(1));
        for (int depth = 0; depth < 4; ++depth) {
            std::vector<Op*> nextLevel;
            for (Op* op : searchOps) {
                if (!op) continue;
                if (strcmp(op->Class(), "SpectralEnvLight") == 0)
                    _allEnvLights.push_back(static_cast<SpectralEnvLight*>(op));
                if (strcmp(op->Class(), "SpectralStudioLight") == 0)
                    _allStudioLights.push_back(static_cast<SpectralStudioLight*>(op));
                for (int i = 0; i < op->inputs(); ++i) {
                    Op* up = op->input(i);
                    if (up) nextLevel.push_back(up);
                }
            }
            searchOps = nextLevel;
        }
    }

    // Cache first of each for legacy param reads
    _cachedEnvLight = !_allEnvLights.empty() ? _allEnvLights[0] : nullptr;
    _cachedStudioLight = !_allStudioLights.empty() ? _allStudioLights[0] : nullptr;

    // Remove previous rig lights but preserve scene input lights.
    // SpectralEnvLight is additive — scene graph lights (GeoDiskLight etc.) are kept.
    auto& lights = _scene->GetLights();
    std::vector<pxr::SpectralLight> sceneLights;
    for (const auto& L : lights) {
        if (!L.name.empty()) sceneLights.push_back(L);
    }
    _scene->ClearLights();
    for (const auto& L : sceneLights) _scene->AddLight(L);

    // ---- Additive env lights: each SpectralEnvLight contributes sun + sky + HDRI ----
    for (SpectralEnvLight* el : _allEnvLights) {
        if (!el) continue;

        double skyMix = 1.0;  // env light node is connected — always active

        // Copy params for volume env lighting from first env light
        if (el == _allEnvLights[0]) {
            _vdbEnvIntensity = el->envIntensity;
            _vdbEnvDiffuse = el->envDiffuse;
            _vdbEnvMode = el->envMode;
            _vdbEnvVirtualLights = el->envVirtualLights;
            _vdbUseReSTIR = el->useReSTIR;
        }

        // Sun/sky from this env light
        if (el->skyPreset > 0) {
            double sunR, sunG, sunB, skyR, skyG, skyB;
            sunColorFromElevation(el->sunElevation, 2.5, sunR, sunG, sunB);
            skyColorFromElevation(el->sunElevation, 2.5, skyR, skyG, skyB);
            GfVec3f sunDir = dirFromElevAzim(el->sunElevation, el->sunAzimuth);

            double sunPow = el->sunIntensity * el->sunIntensity;
            double elevFactor = std::max(0.1, std::min(el->sunElevation / 12.0, 1.0));
            double si = sunPow * elevFactor * skyMix;
            // Ensure minimum sun brightness at low elevations (only if sun enabled)
            if (el->sunIntensity > 0.01)
                si = std::max(si, 0.5 * skyMix);
            if (si > 0.001) {
                SpectralLight sun;
                if (el->sunShadowSoftness > 0.01) {
                    sun.type = SpectralLight::Type::Sphere;
                    float dist = 500.f;
                    sun.position = GfVec3f(sunDir[0]*-dist, sunDir[1]*-dist, sunDir[2]*-dist);
                    sun.radius = float(el->sunShadowSoftness * 50.f);
                } else {
                    sun.type = SpectralLight::Type::Distant;
                    sun.direction = sunDir;
                }
                sun.color = GfVec3f(float(sunR * si), float(sunG * si), float(sunB * si));
                sun.illuminant = SpectralLight::Illuminant::RGB;
                sun.intensity = 1.f;
                _scene->AddLight(sun);
                SLOG("SpectralRender: env sky sun type=%s softness=%.2f radius=%.1f\n",
                    sun.type == SpectralLight::Type::Sphere ? "Sphere" : "Distant",
                    el->sunShadowSoftness, sun.radius);
            }

            double ski = el->skyIntensity * skyMix;
            // Ensure minimum sky ambient (only if sky enabled)
            if (el->skyIntensity > 0.01)
                ski = std::max(ski, 0.1 * skyMix);
            if (ski > 0.001) {
                SpectralLight sky;
                sky.type = SpectralLight::Type::Dome;
                sky.color = GfVec3f(float(skyR * ski), float(skyG * ski), float(skyB * ski));
                sky.intensity = 1.f;
                sky.illuminant = SpectralLight::Illuminant::RGB;

                // Planet sky colour overrides
                if (el->skyPreset == 13) sky.color = GfVec3f(float(0.8*ski), float(0.55*ski), float(0.3*ski));
                else if (el->skyPreset == 14) sky.color = GfVec3f(float(0.7*ski), float(0.45*ski), float(0.15*ski));
                else if (el->skyPreset == 15) sky.color = GfVec3f(float(0.9*ski), float(0.2*ski), float(0.15*ski));
                else if (el->skyPreset == 16) sky.color = GfVec3f(float(0.9*ski), float(0.7*ski), float(0.35*ski));
                else if (el->skyPreset == 17) sky.color = GfVec3f(float(0.3*ski), float(0.4*ski), float(0.9*ski));

                _scene->AddLight(sky);
            }
        }

        // HDRI from this env light (file knob OR input pipe)
        bool hasHdri = false;
        int texId = -1;

        // Check input pipe first (input 1 = HDRI Iop)
        if (el->inputs() > 1 && el->input(1)) {
            Iop* hdriIop = dynamic_cast<Iop*>(el->input(1));
            if (hdriIop) {
                try {
                    hdriIop->validate(true);
                    int W = hdriIop->info().w(), H = hdriIop->info().h();
                    if (W > 0 && H > 0) {
                        hdriIop->request(0, 0, W, H, Mask_RGB, 1);
                        pxr::SpectralTexture tex;
                        tex._pixels.resize(size_t(W) * H * 3);
                        tex._width = W; tex._height = H; tex._channels = 3;
                        tex._path = "input_pipe";
                        for (int y = 0; y < H; ++y) {
                            Row row(0, W);
                            hdriIop->get(y, 0, W, Mask_RGB, row);
                            const float* rp = row[Chan_Red];
                            const float* gp = row[Chan_Green];
                            const float* bp = row[Chan_Blue];
                            int storeY = H - 1 - y;  // flip Y (Nuke bottom-up → top-down)
                            for (int x = 0; x < W; ++x) {
                                size_t idx = (size_t(storeY) * W + x) * 3;
                                tex._pixels[idx]   = rp ? rp[x] : 0.f;
                                tex._pixels[idx+1] = gp ? gp[x] : 0.f;
                                tex._pixels[idx+2] = bp ? bp[x] : 0.f;
                            }
                        }
                        texId = _scene->AddTexture(std::move(tex));
                        hasHdri = true;
                        SLOG("SpectralRender: HDRI from input pipe (%dx%d)\n", W, H);
                    }
                } catch (...) {
                    SLOG("SpectralRender: HDRI input pipe failed\n");
                }
            }
        }

        // Fall back to file knob
        if (!hasHdri && el->hdriFile && strlen(el->hdriFile) > 0) {
            texId = _scene->LoadTexture(el->hdriFile);
            hasHdri = (texId >= 0);
        }

        if (hasHdri && texId >= 0) {
            const auto* tex = _scene->GetTexture(texId);
            if (tex && tex->IsValid()) {
                SpectralLight hdriDome;
                hdriDome.type = SpectralLight::Type::Dome;
                hdriDome.color = GfVec3f(1.f);
                // Apply ND filter: each stop halves brightness
                float ndAtten = (el->ndFilter > 0.01) ? float(std::pow(2.0, -el->ndFilter)) : 1.f;
                hdriDome.intensity = float(el->hdriIntensity) * ndAtten;
                hdriDome.envTexId = texId;
                hdriDome.envWidth = tex->GetWidth();
                hdriDome.envHeight = tex->GetHeight();
                hdriDome.envPixels = tex->_pixels.data();
                hdriDome.envRotation = float(el->hdriRotate);
                hdriDome.envShadowSoftness = float(el->hdriShadowSoftness);
                hdriDome.ComputeEnvAverage();
                _scene->AddLight(hdriDome);
                SLOG("SpectralRender: HDRI dome (%dx%d) intensity=%.1f nd=%.1f stops (effective=%.3f) avg=(%.3f,%.3f,%.3f) cdf=%s sh=%s vlights=%d\n",
                        hdriDome.envWidth, hdriDome.envHeight, el->hdriIntensity, el->ndFilter,
                        hdriDome.intensity,
                        hdriDome.envAvgColor[0], hdriDome.envAvgColor[1], hdriDome.envAvgColor[2],
                        hdriDome.envHasCDF ? "yes" : "no",
                        hdriDome.envHasSH ? "yes" : "no",
                        (int)hdriDome.envVirtualLights.size());
                for (size_t vi = 0; vi < hdriDome.envVirtualLights.size(); ++vi) {
                    auto& vl = hdriDome.envVirtualLights[vi];
                    SLOG("  vlight[%zu] dir=(%.2f,%.2f,%.2f) rgb=(%.2f,%.2f,%.2f)\n",
                            vi, vl.direction[0], vl.direction[1], vl.direction[2],
                            vl.color[0], vl.color[1], vl.color[2]);
                }
            }
        }
    }

    // ---- Additive studio lights: each SpectralStudioLight contributes key + fill + rim ----
    for (SpectralStudioLight* sl : _allStudioLights) {
        if (!sl) continue;

        double m = sl->mix;
        if (m < 0.001) continue;

        double ki = sl->keyIntensity * m;

        // Key light
        if (ki > 0.001) {
            SpectralLight key;
            GfVec3f keyDir = dirFromElevAzim(sl->keyElevation, sl->keyAzimuth);
            if (sl->shadowSoftness > 0.01) {
                key.type = SpectralLight::Type::Sphere;
                float dist = 500.f;
                key.position = GfVec3f(keyDir[0]*-dist, keyDir[1]*-dist, keyDir[2]*-dist);
                key.radius = float(sl->shadowSoftness * 50.f);
            } else {
                key.type = SpectralLight::Type::Distant;
                key.direction = keyDir;
            }
            key.color = GfVec3f(float(ki), float(ki * 0.95), float(ki * 0.9));
            key.illuminant = SpectralLight::Illuminant::RGB;
            key.intensity = 1.f;
            _scene->AddLight(key);
        }

        // Fill light
        double fi = ki * sl->fillRatio;
        if (fi > 0.001) {
            SpectralLight fill;
            fill.type = SpectralLight::Type::Distant;
            fill.direction = dirFromElevAzim(sl->keyElevation * 0.5, sl->keyAzimuth + 180);
            fill.color = GfVec3f(float(fi * 0.9), float(fi * 0.92), float(fi));
            fill.illuminant = SpectralLight::Illuminant::RGB;
            fill.intensity = 1.f;
            _scene->AddLight(fill);
        }

        // Rim light
        double ri = sl->rimIntensity * m;
        if (ri > 0.001) {
            SpectralLight rim;
            rim.type = SpectralLight::Type::Distant;
            rim.direction = dirFromElevAzim(sl->keyElevation + 15, sl->keyAzimuth + 160);
            rim.color = GfVec3f(float(ri));
            rim.illuminant = SpectralLight::Illuminant::RGB;
            rim.intensity = 1.f;
            _scene->AddLight(rim);
        }
    }

    // ---- Fallback: use SpectralRender's own lighting knobs if no light nodes connected ----
    if (_useBuiltinLight && _allEnvLights.empty() && _allStudioLights.empty()) {
        // Sun/sky from local knobs
        if (_skyPreset > 0 && _skyMix > 0.001) {
            double m = _skyMix;
            double sunR, sunG, sunB, skyR, skyG, skyB;
            sunColorFromElevation(_sunElevation, _turbidity, sunR, sunG, sunB);
            skyColorFromElevation(_sunElevation, _turbidity, skyR, skyG, skyB);
            GfVec3f sunDir = dirFromElevAzim(_sunElevation, _sunAzimuth);

            double sunPow = _sunIntensity * _sunIntensity;
            double si = sunPow * std::max(0.1, std::min(_sunElevation / 12.0, 1.0)) * m;
            if (si > 0.001) {
                SpectralLight sun;
                if (_shadowSoftness > 0.01) {
                    sun.type = SpectralLight::Type::Sphere;
                    float dist = 500.f;
                    sun.position = GfVec3f(sunDir[0]*-dist, sunDir[1]*-dist, sunDir[2]*-dist);
                    sun.radius = float(_shadowSoftness * 50.f);
                } else {
                    sun.type = SpectralLight::Type::Distant;
                    sun.direction = sunDir;
                }
                sun.color = GfVec3f(float(sunR * si), float(sunG * si), float(sunB * si));
                sun.illuminant = SpectralLight::Illuminant::RGB;
                sun.intensity = 1.f;
                _scene->AddLight(sun);
            }

            double ski = _skyIntensity * m;
            if (ski > 0.001) {
                SpectralLight sky;
                sky.type = SpectralLight::Type::Dome;
                sky.color = GfVec3f(float(skyR * ski), float(skyG * ski), float(skyB * ski));
                sky.illuminant = SpectralLight::Illuminant::RGB;
                sky.intensity = 1.f;
                _scene->AddLight(sky);
            }
        }

        // Studio from local knobs
        if (_studioPreset > 0 && _studioMix > 0.001) {
            double m = _studioMix;
            double ki = _studioKeyIntensity * m;
            if (ki > 0.001) {
                SpectralLight key;
                GfVec3f keyDir = dirFromElevAzim(_studioKeyElevation, _studioKeyAzimuth);
                if (_shadowSoftness > 0.01) {
                    key.type = SpectralLight::Type::Sphere;
                    float dist = 500.f;
                    key.position = GfVec3f(keyDir[0]*-dist, keyDir[1]*-dist, keyDir[2]*-dist);
                    key.radius = float(_shadowSoftness * 50.f);
                } else {
                    key.type = SpectralLight::Type::Distant;
                    key.direction = keyDir;
                }
                key.color = GfVec3f(float(ki), float(ki * 0.95), float(ki * 0.9));
                key.illuminant = SpectralLight::Illuminant::RGB;
                key.intensity = 1.f;
                _scene->AddLight(key);
            }
            double fi = ki * _studioFillRatio;
            if (fi > 0.001) {
                SpectralLight fill;
                fill.type = SpectralLight::Type::Distant;
                fill.direction = dirFromElevAzim(_studioKeyElevation * 0.5, _studioKeyAzimuth + 180);
                fill.color = GfVec3f(float(fi * 0.9), float(fi * 0.92), float(fi));
                fill.illuminant = SpectralLight::Illuminant::RGB;
                fill.intensity = 1.f;
                _scene->AddLight(fill);
            }
            double ri = _studioRimIntensity * m;
            if (ri > 0.001) {
                SpectralLight rim;
                rim.type = SpectralLight::Type::Distant;
                rim.direction = dirFromElevAzim(_studioKeyElevation + 15, _studioKeyAzimuth + 160);
                rim.color = GfVec3f(float(ri));
                rim.illuminant = SpectralLight::Illuminant::RGB;
                rim.intensity = 1.f;
                _scene->AddLight(rim);
            }
        }

        // HDRI from local knobs
        if (_hdriFile && strlen(_hdriFile) > 0) {
            int texId = _scene->LoadTexture(_hdriFile);
            if (texId >= 0) {
                const auto* tex = _scene->GetTexture(texId);
                if (tex && tex->IsValid()) {
                    SpectralLight hdriDome;
                    hdriDome.type = SpectralLight::Type::Dome;
                    hdriDome.color = GfVec3f(1.f);
                    hdriDome.intensity = float(_hdriIntensity);
                    hdriDome.envTexId = texId;
                    hdriDome.envWidth = tex->GetWidth();
                    hdriDome.envHeight = tex->GetHeight();
                    hdriDome.envPixels = tex->_pixels.data();
                    hdriDome.envRotation = float(_hdriRotate);
                    hdriDome.ComputeEnvAverage();
                    _scene->AddLight(hdriDome);
                }
            }
        }
    }

    // Safety net: ensure scene has minimum illumination for spectral rendering.
    // Only when built-in light is enabled.
    //
    // Deliberately skipped when scanlineCompat is on: in that mode, zero
    // lights is a feature, not a bug -- it triggers the constant-shader
    // path (flat material colour) matching Nuke's ScanlineRender. Injecting
    // a fallback dome here would defeat that contract.
    if (_useBuiltinLight && !_scanlineCompat) {
    // Very dim/unbalanced lighting causes severe noise in GPU spectral path.
    {
        const auto& allLights = _scene->GetLights();
        float totalLum = 0.f;
        for (const auto& L : allLights) {
            float lum = L.color[0] * 0.2126f + L.color[1] * 0.7152f + L.color[2] * 0.0722f;
            totalLum += lum * L.intensity;
        }
        if (totalLum < 0.5f) {
            // Add a subtle balanced dome to prevent black/noisy renders
            pxr::SpectralLight fallbackDome;
            fallbackDome.type = pxr::SpectralLight::Type::Dome;
            float fill = std::max(0.1f, 0.5f - totalLum);
            fallbackDome.color = GfVec3f(fill * 0.8f, fill * 0.85f, fill);
            fallbackDome.illuminant = pxr::SpectralLight::Illuminant::RGB;
            fallbackDome.intensity = 1.f;
            _scene->AddLight(fallbackDome);
            SLOG("SpectralRender: added fallback dome (totalLum=%.3f fill=%.3f)\n", totalLum, fill);
        }
    }
    }  // _useBuiltinLight

    // Log all lights
    {
        const auto& allLights = _scene->GetLights();
        SLOG("SpectralRender: %zu lights in scene:\n", allLights.size());
        for (size_t i = 0; i < allLights.size(); ++i) {
            const auto& L = allLights[i];
            const char* typeNames[] = {"Distant","Sphere","Rect","Dome","Spot"};
            int ti = std::min(int(L.type), 4);
            SLOG("  [%zu] %s '%s' color=(%.2f,%.2f,%.2f) intensity=%.2f env=%s\n",
                    i, typeNames[ti], L.name.c_str(),
                    L.color[0], L.color[1], L.color[2], L.intensity,
                    L.envPixels ? "HDRI" : "none");
        }
    }
}

void SpectralRenderIop::_EnsureFrameRendered()
{
    // Frame already rendered — async quality thread handles refinement
    if (_frameReady.load()) return;

    std::lock_guard<std::recursive_mutex> lock(_renderMutex);
    if (_frameReady.load()) return;

    auto tEnsureStart = std::chrono::high_resolution_clock::now();

    // Load volume — upgrade to full 128³ for render if currently at 64³ preview
    _LoadVDB();          // ensures volume is loaded (may be 64³ preview)
    // Only upgrade resolution for single-VDB path (VolMerge handles its own resolution)
    if (_volumes.empty()) {
        _LoadVDBForRender(); // upgrades to 128³ if needed
    }

    // Skip if no scene content (no geometry AND no volume)
    bool hasGeometry = _scene && _scene->TotalTriangles() > 0;
    bool hasVolume = !_volumes.empty() || (_volume && _volume->IsValid());
    if (!hasGeometry && !hasVolume) {
        // Clear framebuffer to black/transparent so viewer updates
        if (!_frameBuffer.empty())
            std::fill(_frameBuffer.begin(), _frameBuffer.end(), 0.f);
        _frameReady.store(true);
        return;
    }

    // Create scene if needed (volume-only, no geometry loaded)
    if (!_scene) _scene = std::make_unique<pxr::SpectralScene>();

    // Use the format from info_ (set by _validate from BG or format knob)
    const Format* fmtPtr = &(info_.format());
    if (!fmtPtr) { _frameReady.store(true); return; }
    const unsigned int fullW = static_cast<unsigned int>(fmtPtr->width());
    const unsigned int fullH = static_cast<unsigned int>(fmtPtr->height());
    if (fullW == 0 || fullH == 0) { _frameReady.store(true); return; }

    // Proxy scale: 0=1/4, 1=1/2, 2=3/4, 3=full
    static const float proxyScales[] = { 0.25f, 0.5f, 0.75f, 1.0f };
    float proxyScale = (_proxyMode >= 0 && _proxyMode <= 3) ? proxyScales[_proxyMode] : 1.0f;
    const unsigned int W = std::max(1u, static_cast<unsigned int>(fullW * proxyScale));
    const unsigned int H = std::max(1u, static_cast<unsigned int>(fullH * proxyScale));

    _fbWidth  = W;
    _fbHeight = H;
    _fbFullWidth  = fullW;
    _fbFullHeight = fullH;
    _frameBuffer.assign(size_t(W) * H * 4, 0.f);
    _depthBuffer.assign(size_t(W) * H, 1e30f);
    _objectIdBuffer.assign(size_t(W) * H, 0.f);
    _materialIdBuffer.assign(size_t(W) * H, 0.f);
    _aoBuffer.assign(size_t(W) * H, 1.f);
    // Topographic wireframe in height / custom-direction mode needs the
    // world-space position buffer. Force it on even if the AOV checkbox is
    // off, otherwise topo falls back to normal.y (which is slope, not height).
    const bool wireTopoNeedsPos = _wireframeEnable && _wireStyle == 5 &&
                                  (_topoDirection == 0 || _topoDirection == 1);
    if (_aovNormals || _wireframeEnable)
        _normalBuffer.assign(size_t(W) * H * 3, 0.f); else _normalBuffer.clear();
    if (_aovPosition || wireTopoNeedsPos)
        _posBuffer.assign(size_t(W) * H * 3, 0.f);    else _posBuffer.clear();
    if (_aovPRef)     _pRefBuffer.assign(size_t(W) * H * 3, 0.f);   else _pRefBuffer.clear();
    if (_aovUV || _wireframeEnable)
        _uvBuffer.assign(size_t(W) * H * 2, 0.f);     else _uvBuffer.clear();
    if (_aovAlbedo)   _albedoBuffer.assign(size_t(W) * H * 3, 0.f); else _albedoBuffer.clear();
    if (_aovDirect)   _directBuffer.assign(size_t(W) * H * 3, 0.f); else _directBuffer.clear();
    if (_aovIndirect) _indirectBuffer.assign(size_t(W) * H * 3, 0.f); else _indirectBuffer.clear();
    if (_aovEmission) _emissionBuffer.assign(size_t(W) * H * 3, 0.f); else _emissionBuffer.clear();
    if (_aovDiffuseDirect)   _diffuseDirectBuffer.assign(size_t(W) * H * 3, 0.f);   else _diffuseDirectBuffer.clear();
    if (_aovSpecularDirect)  _specularDirectBuffer.assign(size_t(W) * H * 3, 0.f);  else _specularDirectBuffer.clear();
    if (_aovDiffuseIndirect) _diffuseIndirectBuffer.assign(size_t(W) * H * 3, 0.f); else _diffuseIndirectBuffer.clear();
    if (_aovSpecularIndirect) _specularIndirectBuffer.assign(size_t(W) * H * 3, 0.f); else _specularIndirectBuffer.clear();
    if (_aovTransmission)    _transmissionBuffer.assign(size_t(W) * H * 3, 0.f);    else _transmissionBuffer.clear();
    _cryptoObjectBuffer.assign(size_t(W) * H * 4, 0.f);  // RGBA: (hash0, cov0, hash1, cov1)

    SpectralCamera cam = _camera;
    cam.imageWidth  = W;
    cam.imageHeight = H;
    cam.pixelAspect = fmtPtr->pixel_aspect();

    // Motion blur shutter — normalize to [0,1] for Embree
    // shutterOpen/Close are relative to frame (e.g. -0.5/0.5)
    // Embree expects [0,1] where 0=first vertex buffer, 1=second
    if (_shutterOpen != _shutterClose) {
        cam.shutterOpen  = 0.f;
        cam.shutterClose = 1.f;
    } else {
        cam.shutterOpen  = 0.f;
        cam.shutterClose = 0.f;
    }

    cam.adaptiveThreshold = _adaptiveThreshold;
    cam.refractionBounces = _refractionBounces;
    cam.blueNoise = _blueNoise;
    cam.scanlineCompat = _scanlineCompat;
    cam.neutralBalance = _neutralBalance;
    cam.projectionMode = _projectionMode;
    cam.edgeSamples = _edgeSamples;
    cam.wireframeEnable = _wireframeEnable;
    cam.wireThickness = float(_wireThickness);
    cam.wireOpacity = float(_wireOpacity);
    cam.wireColor[0] = float(_wireColor[0]);
    cam.wireColor[1] = float(_wireColor[1]);
    cam.wireColor[2] = float(_wireColor[2]);
    cam.wireDashed = _wireDashed;
    cam.wireDashLength = float(_wireDashLength);
    cam.wireGapLength = float(_wireGapLength);
    cam.wireNth = _wireNth;
    cam.wireStyle = _wireStyle;
    cam.shadowCatcherMatIds = _shadowCatcherMatIds;
    cam.noShadowCastMatIds  = _noShadowCastMatIds;
    cam.fStop = _fStop;
    cam.focusDistance = _focusDistance;
    cam.volumeSpp = _volumeSpp;

    // Progressive rendering: first pass is a fast preview
    int renderSpp = _samples;
    bool isPreviewPass = false;
    // Camera motion blur: multiply samples for quality
    if (_cameraMblur && _camera.cameraMblur) {
        renderSpp = std::max(renderSpp, renderSpp * _cameraMblurQuality / 4);
    }

    // Determine render device (needed before preview decision)
    bool useGPU = false;
#ifdef SPECTRAL_HAS_OPTIX
    if (_deviceMode == 1) {          // gpu
        useGPU = true;
    } else if (_deviceMode == 2) {   // auto
        useGPU = SpectralIntegrator::IsGPUAvailable();
    }
#endif

    // Object motion blur: not yet supported on GPU (OptiX motion GAS +
    // rayTime threading through all optixTrace call sites is a follow-up
    // patch). Fall back to CPU cleanly with a one-time log so the user
    // isn't confused by a silent missing feature.
    if (useGPU && _objectMotionBlur && _shutterOpen != _shutterClose) {
        static bool s_warnedMblurFallback = false;
        if (!s_warnedMblurFallback) {
            SLOG("SpectralRender: object motion blur active -- GPU motion GAS "
                 "not yet implemented, falling back to CPU for this render. "
                 "Disable object motion blur or zero the shutter interval to "
                 "render on GPU.\n");
            s_warnedMblurFallback = true;
        }
        useGPU = false;
    }

    // Auto-preview disabled — GPU renders fast enough at full spp (28-100ms).
    // spp=1 preview showed normals-as-color that persisted because Nuke's
    // pull-based Iop model doesn't rescan after async quality completes.
    // if (hasVolumeScene && _progressiveSppDone == 0 && _samples > 1 && useGPU) {
    //     renderSpp = 1;
    //     isPreviewPass = true;
    // }
    if (_progressive && _progressiveSppDone == 0 && _samples > 2 && useGPU) {
        // Only use progressive for explicitly enabled progressive mode
        renderSpp = std::max(1, _samples / 4);  // quarter spp, not 1
        isPreviewPass = true;
    }

    const char* deviceStr = useGPU ? "GPU" : "CPU";
    const char* passStr = isPreviewPass ? " [preview]" : "";
    if (W != fullW || H != fullH)
        SLOG("SpectralRender: rendering %dx%d (proxy of %dx%d), %zu tris, %d spp, device=%s%s\n",
                W, H, fullW, fullH, _scene->TotalTriangles(), renderSpp, deviceStr, passStr);
    else
        SLOG("SpectralRender: rendering %dx%d, %zu tris, %d spp, device=%s%s\n",
                W, H, _scene->TotalTriangles(), renderSpp, deviceStr, passStr);

    // Caustics disabled — photon mapping removed for stability.
    // Will be re-implemented with progressive photon mapping in Phase 8d.
    const pxr::SpectralPhotonMap* pmap = nullptr;

    // Add built-in sun/sky and studio lights
    _BuildLightRig();

    // Volume already loaded at top of _EnsureFrameRendered

    // Build volume pointer array for multi-volume rendering
    // Re-load volumes at render resolution and re-apply materials
    if (!_volumes.empty() && _cachedVolMerge) {
        auto tVolStart = std::chrono::high_resolution_clock::now();
        int masterMaxRes = _GetMasterMaxRes();
        if (isPreviewPass) {
            // Scrub quality: cap preview resolution for fast interaction
            if (_scrubQuality == 0) masterMaxRes = std::min(masterMaxRes, 64);       // Fast: 64³ density-only
            else if (_scrubQuality == 1) masterMaxRes = std::min(masterMaxRes, 128);  // Medium: 128³
            // scrubQuality == 2: no cap, full render resolution
        }
        auto entries = _cachedVolMerge->GetVolumes(int(outputContext().frame()), masterMaxRes);
        auto tVolLoad = std::chrono::high_resolution_clock::now();

        // Replace _volumes with render-resolution volumes from VolMerge
        _volumes.clear();
        for (auto& e : entries) {
            if (e.volume && e.volume->IsValid()) {
                _volumes.push_back(e.volume);
                // Bbox lock: stabilize position (post-transform)
                if (e.vdbRead && e.vdbRead->GetLockBbox() && _hasRefVolCenter) {
                    pxr::GfVec3f curCenter = (e.volume->GetBboxMin() + e.volume->GetBboxMax()) * 0.5f;
                    pxr::GfVec3f offset = _refVolCenter - curCenter;
                    if (e.volume->hasTransform) {
                        e.volume->xfBboxMin += offset;
                        e.volume->xfBboxMax += offset;
                        e.volume->_center += offset;
                    } else {
                        e.volume->bboxMin += offset;
                        e.volume->bboxMax += offset;
                    }
                }
            }
        }
        _volume = _volumes.empty() ? nullptr : _volumes[0];
        {
            auto tVolEnd = std::chrono::high_resolution_clock::now();
            auto msLoad = std::chrono::duration_cast<std::chrono::milliseconds>(tVolLoad - tVolStart).count();
            auto msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(tVolEnd - tVolStart).count();
            SLOG("SpectralRender: VolMerge reload — vdb_load=%lldms materials=%lldms total=%lldms (%d vols)\n",
                 msLoad, msTotal - msLoad, msTotal, (int)_volumes.size());
        }

        // Collect materials from VolMerge inputs
        std::vector<SpectralVolumeMaterial*> volMats;
        for (int inp = 0; inp < _cachedVolMerge->inputs(); ++inp) {
            Op* up = _cachedVolMerge->input(inp);
            if (!up || up->node_disabled()) continue;
            up->validate(true);
            Op* cur = up;
            for (int d = 0; d < 6 && cur; ++d) {
                if (strcmp(cur->Class(), "SpectralVolumeMaterial") == 0) {
                    volMats.push_back(static_cast<SpectralVolumeMaterial*>(cur));
                    break;
                }
                cur = (cur->inputs() > 0) ? cur->input(0) : nullptr;
            }
        }
        for (size_t vi = 0; vi < _volumes.size() && vi < entries.size(); ++vi) {
            auto& e = entries[vi];
            bool appliedMat = false;
            if (e.vdbRead && e.vdbRead->inputs() > 0 && e.vdbRead->input(0)) {
                Op* matOp = e.vdbRead->input(0);
                if (matOp) matOp->validate(true);
                Op* cur = matOp;
                for (int d = 0; d < 4 && cur; ++d) {
                    if (strcmp(cur->Class(), "SpectralVolumeMaterial") == 0) {
                        _applyVolumeMaterialDirect(_volumes[vi],
                            static_cast<SpectralVolumeMaterial*>(cur));
                        appliedMat = true;
                        break;
                    }
                    cur = (cur->inputs() > 0) ? cur->input(0) : nullptr;
                }
            }
            if (!appliedMat) {
                if ((int)vi < (int)volMats.size())
                    _applyVolumeMaterialDirect(_volumes[vi], volMats[vi]);
                else if (!volMats.empty())
                    _applyVolumeMaterialDirect(_volumes[vi], volMats.back());
            }
        }
    }
    // Non-VolMerge scn chain: upgrade from preview res to render res
    else if (!_volumes.empty() && !_cachedVdbReads.empty()) {
        int masterMaxRes = _GetMasterMaxRes();
        int renderFrame = int(outputContext().frame());
        _volumes.clear();
        for (auto* vdb : _cachedVdbReads) {
            if (!vdb || vdb->node_disabled()) continue;
            vdb->validate(true);
            int nodeRes = vdb->GetMaxRes();
            int effectiveRes = std::min(masterMaxRes, nodeRes);
            auto vol = vdb->GetVolumeAtFrame(renderFrame, effectiveRes);
            if (vol && vol->IsValid()) {
                _applyVolumeShading(vol, vdb);
                _volumes.push_back(vol);
            }
        }
        _volume = _volumes.empty() ? nullptr : _volumes[0];
    }

    std::vector<const pxr::SpectralVolume*> volPtrs;
    for (auto& v : _volumes) if (v) volPtrs.push_back(v.get());
    if (volPtrs.empty() && _volume) volPtrs.push_back(_volume.get());

    // UV projection: rasterize triangles in UV space for UV render mode
    if (_projectionMode == 1 && _scene) {
        _uvTriIndexBuf.assign(size_t(W) * H, -1);
        _uvBaryUBuf.assign(size_t(W) * H, 0.f);
        _uvBaryVBuf.assign(size_t(W) * H, 0.f);

        const auto& meshes = _scene->GetMeshes();
        int globalTriIdx = 0;
        for (const auto& mesh : meshes) {
            for (const auto& tri : mesh.second.triangles) {
                // Triangle UV coordinates in pixel space
                float u0 = tri.uv0[0] * W, v0 = (1.f - tri.uv0[1]) * H;
                float u1 = tri.uv1[0] * W, v1 = (1.f - tri.uv1[1]) * H;
                float u2 = tri.uv2[0] * W, v2 = (1.f - tri.uv2[1]) * H;

                // Bounding box in pixel space
                int minX = std::max(0, (int)std::floor(std::min({u0, u1, u2})));
                int maxX = std::min((int)W - 1, (int)std::ceil(std::max({u0, u1, u2})));
                int minY = std::max(0, (int)std::floor(std::min({v0, v1, v2})));
                int maxY = std::min((int)H - 1, (int)std::ceil(std::max({v0, v1, v2})));

                // Edge function rasterization
                float denom = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0);
                if (std::abs(denom) < 1e-8f) { globalTriIdx++; continue; }
                float invDenom = 1.f / denom;

                for (int py2 = minY; py2 <= maxY; ++py2) {
                    for (int px2 = minX; px2 <= maxX; ++px2) {
                        float cx = px2 + 0.5f, cy = py2 + 0.5f;
                        float bU = ((cx - u0) * (v2 - v0) - (u2 - u0) * (cy - v0)) * invDenom;
                        float bV = ((u1 - u0) * (cy - v0) - (cx - u0) * (v1 - v0)) * invDenom;
                        float bW = 1.f - bU - bV;
                        if (bU >= 0.f && bV >= 0.f && bW >= 0.f) {
                            size_t idx = size_t(py2) * W + px2;
                            _uvTriIndexBuf[idx] = globalTriIdx;
                            _uvBaryUBuf[idx] = bU;
                            _uvBaryVBuf[idx] = bV;
                        }
                    }
                }
                globalTriIdx++;
            }
        }
        SLOG("SpectralRender: UV rasterization complete (%dx%d, %d triangles)\n", W, H, globalTriIdx);
    }

    // Set UV buffer pointers after rasterization completes
    if (_projectionMode == 1 && !_uvTriIndexBuf.empty()) {
        cam.uvTriIndex = _uvTriIndexBuf.data();
        cam.uvBaryU    = _uvBaryUBuf.data();
        cam.uvBaryV    = _uvBaryVBuf.data();
        cam.uvBufSize  = _uvTriIndexBuf.size();
    }

    // Cache render params for strip rendering
    _renderCam = cam;
    _renderSpp = renderSpp;
    _renderUseGPU = useGPU;
    _renderVolPtrs = volPtrs;

    // Init strip state
    int numStrips = (H + kStripHeight - 1) / kStripHeight;
    _stripRendered.assign(numStrips, false);
    _postProcessDone = false;

    _sceneReady.store(true);

    auto tSceneEnd = std::chrono::high_resolution_clock::now();
    double sceneMs = std::chrono::duration<double, std::milli>(tSceneEnd - tEnsureStart).count();
    SLOG("SpectralRender: scene ready in %.1f ms — %d strips of %d rows\n",
            sceneMs, numStrips, kStripHeight);
}

// ---------------------------------------------------------------------------
// _EnsureSceneReady — alias for backward compatibility
// ---------------------------------------------------------------------------
void SpectralRenderIop::_EnsureSceneReady()
{
    if (_sceneReady.load()) return;

    std::lock_guard<std::recursive_mutex> lock(_renderMutex);
    if (_sceneReady.load()) return;

    _EnsureFrameRendered();
}

// ---------------------------------------------------------------------------
// _RenderStrip — render on demand (full frame on first strip request)
// ---------------------------------------------------------------------------
void SpectralRenderIop::_RenderStrip(int stripIdx)
{
    std::lock_guard<std::mutex> lock(_stripMutex);
    if (_stripRendered[stripIdx]) return;

    // Render entire frame on first strip request
    // (GPU is fast enough; CPU uses parallel execution internally)
    if (!_frameReady.load()) {
        _frameReady.store(true);

        const int W = static_cast<int>(_fbWidth);
        const int H = static_cast<int>(_fbHeight);

        auto tStart = std::chrono::high_resolution_clock::now();

#ifdef SPECTRAL_HAS_OPTIX
        if (_renderUseGPU) {
            SpectralIntegrator::RenderFrameGPU(*_scene, _renderCam, _frameBuffer.data(),
                                                _renderSpp, _depthBuffer.data(), _maxBounces,
                                                _colorSpace,
                                                _renderVolPtrs.empty() ? nullptr : _renderVolPtrs.data(),
                                                (int)_renderVolPtrs.size(),
                                                nullptr, 4);
        } else
#endif
        {
            SpectralIntegrator::AOVBuffers aovBufs;
            const bool wireTopoNeedsPosCpu = _wireframeEnable && _wireStyle == 5 &&
                                             (_topoDirection == 0 || _topoDirection == 1);
            aovBufs.normal   = (_aovNormals || _wireframeEnable) ? _normalBuffer.data() : nullptr;
            aovBufs.position = (_aovPosition || wireTopoNeedsPosCpu) ? _posBuffer.data() : nullptr;
            aovBufs.pRef     = _aovPRef     ? _pRefBuffer.data()     : nullptr;
            aovBufs.uv       = (_aovUV || _wireframeEnable) ? _uvBuffer.data() : nullptr;
            aovBufs.albedo   = _aovAlbedo   ? _albedoBuffer.data()   : nullptr;
            aovBufs.direct   = _aovDirect   ? _directBuffer.data()   : nullptr;
            aovBufs.indirect = _aovIndirect ? _indirectBuffer.data() : nullptr;
            aovBufs.emission = _aovEmission ? _emissionBuffer.data() : nullptr;
            aovBufs.diffuseDirect   = _aovDiffuseDirect   ? _diffuseDirectBuffer.data()   : nullptr;
            aovBufs.specularDirect  = _aovSpecularDirect  ? _specularDirectBuffer.data()  : nullptr;
            aovBufs.diffuseIndirect = _aovDiffuseIndirect ? _diffuseIndirectBuffer.data() : nullptr;
            aovBufs.specularIndirect = _aovSpecularIndirect ? _specularIndirectBuffer.data() : nullptr;
            aovBufs.transmission    = _aovTransmission    ? _transmissionBuffer.data()    : nullptr;

            SpectralIntegrator::RenderFrame(*_scene, _renderCam, _frameBuffer.data(),
                                             _renderSpp, _depthBuffer.data(), _maxBounces,
                                             _objectIdBuffer.data(), _materialIdBuffer.data(),
                                             &aovBufs, nullptr, nullptr, _causticRadius,
                                             _colorSpace,
                                             _renderVolPtrs.empty() ? nullptr : _renderVolPtrs.data(),
                                             (int)_renderVolPtrs.size());
        }

        // Denoise
#ifdef SPECTRAL_HAS_OPTIX
        if (_denoise) {
            SpectralIntegrator::DenoiseGPU(W, H, _frameBuffer.data());
        }
#endif

        // Geometry AOV pass — clean, pixel-centred UV / N / P / ID.
        //
        // This matters for both paths:
        //   GPU: the main render doesn't fill these, so the pass is the
        //        only source of them.
        //   CPU: the main render *does* write these inline via
        //        _WriteFirstHitAOVs, but it writes the FIRST sample's
        //        jittered UV, not the pixel centre. Per-pixel sub-pixel
        //        noise makes the wireframe overlay's grid-distance test
        //        noisy -- every style looks wobbly, but it's most
        //        obviously wrong on "solid" (user-reported "CPU solid
        //        line renders as pencil sketch" -- the wobble was
        //        indistinguishable from the pencil wobble). Re-running
        //        with centre-of-pixel rays overwrites the jittered
        //        values with deterministic ones.
        {
            const bool wireTopoNeedsPosGeom = _wireframeEnable && _wireStyle == 5 &&
                                              (_topoDirection == 0 || _topoDirection == 1);
            bool needGeomPass = _aovNormals || _aovPosition || _aovPRef || _aovUV || _aovAlbedo || _wireframeEnable;
            if (needGeomPass) {
                SpectralIntegrator::ComputeGeometryAOVs(
                    *_scene, _renderCam,
                    (_aovNormals || _wireframeEnable) ? _normalBuffer.data() : nullptr,
                    (_aovPosition || wireTopoNeedsPosGeom) ? _posBuffer.data() : nullptr,
                    _aovPRef     ? _pRefBuffer.data()   : nullptr,
                    (_aovUV || _wireframeEnable) ? _uvBuffer.data() : nullptr,
                    _aovAlbedo   ? _albedoBuffer.data() : nullptr,
                    _objectIdBuffer.data(), _materialIdBuffer.data(),
                    nullptr);
            }
        }

        // AO pass
        if (_aoSamples > 0) {
            SpectralIntegrator::ComputeAO(*_scene, _renderCam, _aoBuffer.data(),
                                           _aoSamples, _aoRadius);
        }

        // ─── Edge anti-aliasing post-pass ───
        // Detects geometry edges via objectId discontinuity and supersamples
        if (_edgeSamples > 0 && !_objectIdBuffer.empty() && !_depthBuffer.empty()) {
            const int edgeSpp = _edgeSamples;
            const size_t numPx = size_t(W) * H;

            // Edge detection: objectId or depth discontinuity with 4-neighbors
            std::vector<bool> isEdge(numPx, false);
            for (int y = 1; y < H - 1; ++y) {
                for (int x = 1; x < W - 1; ++x) {
                    size_t idx = y * W + x;
                    float myObj = _objectIdBuffer[idx];
                    float myDepth = _depthBuffer[idx];
                    static const int dx[] = {-1, 1, 0, 0};
                    static const int dy[] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; ++d) {
                        size_t nIdx = (y + dy[d]) * W + (x + dx[d]);
                        if (_objectIdBuffer[nIdx] != myObj) { isEdge[idx] = true; break; }
                        if (myDepth > 0.01f) {
                            float ratio = _depthBuffer[nIdx] / myDepth;
                            if (ratio < 0.85f || ratio > 1.15f) { isEdge[idx] = true; break; }
                        }
                    }
                }
            }

            // Count for log
            int edgeCount = 0;
            for (size_t i = 0; i < numPx; ++i) if (isEdge[i]) edgeCount++;

            // Supersample edge pixels by averaging with jittered neighbor samples
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    size_t idx = y * W + x;
                    if (!isEdge[idx]) continue;

                    float* px = _frameBuffer.data() + idx * 4;
                    float sumR = px[0], sumG = px[1], sumB = px[2], sumA = px[3];
                    int count = 1;

                    // Sample sub-pixel offsets
                    for (int s = 0; s < edgeSpp; ++s) {
                        float offX = (float(s % 4) + 0.5f) / 4.f - 0.5f;
                        float offY = (float(s / 4) + 0.5f) / 4.f - 0.5f;
                        int nx = std::max(0, std::min(W - 1, x + (offX > 0 ? 1 : -1)));
                        int ny = std::max(0, std::min(H - 1, y + (offY > 0 ? 1 : -1)));
                        const float* np = _frameBuffer.data() + (ny * W + nx) * 4;
                        sumR += np[0]; sumG += np[1]; sumB += np[2]; sumA += np[3];
                        count++;
                    }
                    float inv = 1.f / float(count);
                    px[0] = sumR * inv; px[1] = sumG * inv; px[2] = sumB * inv; px[3] = sumA * inv;
                }
            }
            fprintf(stderr, "SpectralRender: edge AA — %d edge pixels smoothed (%d samples)\n", edgeCount, edgeSpp);
        }

        // ─── Wireframe overlay (UV-grid + topo + pencil styles) ───
        if (_wireframeEnable && float(_wireOpacity) > 0.001f && !_uvBuffer.empty()) {
            const float thickness = float(_wireThickness);
            const float opacity   = float(_wireOpacity);
            const float wireR = float(_wireColor[0]);
            const float wireG = float(_wireColor[1]);
            const float wireB = float(_wireColor[2]);
            const int   nth   = std::max(1, _wireNth);
            const int   style = _wireStyle;
            const float gridFreq = float(nth);
            const bool  hasNormals = !_normalBuffer.empty();
            const bool  hasPosition = !_posBuffer.empty();

            // Antialiasing: the overlay runs once after the integrator, so
            // render samples do nothing for it. The previous falloff was a
            // 1-pixel linear ramp, which left plenty of binary-like
            // inclusion changes frame to frame. A smoothstep across a wider
            // band is essentially free and kills most of that flicker.
            //
            // aaMode:  0=off, 1=smooth (1x), 2=soft (1.5x), 3=softest (2.5x)
            // aaWidth: base band width in pixels
            const int   aaMode  = _wireAAMode;
            const float aaBase  = std::max(0.01f, float(_wireAAWidth));
            const float aaBand  = (aaMode == 0) ? 0.f :
                                  (aaMode == 1) ? aaBase :
                                  (aaMode == 2) ? aaBase * 1.5f :
                                                  aaBase * 2.5f;
            auto coverage = [aaMode, aaBand](float pixDist, float thick) -> float {
                if (aaMode == 0) {
                    // Original hard-edged behaviour: 1-pixel linear ramp
                    if (pixDist > thick) return 0.f;
                    if (pixDist > thick - 1.f) return std::max(0.f, thick - pixDist);
                    return 1.f;
                }
                if (pixDist <= thick) return 1.f;
                if (pixDist >= thick + aaBand) return 0.f;
                float t = (pixDist - thick) / aaBand;
                return 1.f - t * t * (3.f - 2.f * t);   // 1 - smoothstep
            };

            // Pencil sketch: precompute noise LUT
            auto pencilNoise = [](int x, int y) -> float {
                unsigned int h = x * 374761393u + y * 668265263u;
                h = (h ^ (h >> 13)) * 1274126177u;
                return float(h & 0xFFFF) / 65535.f;
            };

            // Precompute normalized curvature for topo=curvature mode.
            // The previous inline version had two bugs: (1) getElev didn't
            // handle the curvature case so the screen-space derivative was
            // garbage, and (2) flat regions produced elevation=0 which puts
            // every pixel exactly on the zero-contour — hence "all white".
            // Precomputing means getElev can just index the buffer, and the
            // normalization + flat-region skip below fixes the white-out.
            std::vector<float> curvBuf;
            if (style == 5 && _topoDirection == 2 && hasNormals) {
                curvBuf.assign(size_t(W) * H, 0.f);
                float maxCurv = 0.f;
                for (int cy = 0; cy < H; ++cy) {
                    for (int cx = 0; cx < W; ++cx) {
                        size_t ci = size_t(cy) * W + cx;
                        if (_objectIdBuffer[ci] == 0.f) continue;
                        float nx = _normalBuffer[ci*3+0];
                        float ny = _normalBuffer[ci*3+1];
                        float nz = _normalBuffer[ci*3+2];
                        float sum = 0.f; int nc = 0;
                        for (int dd = 0; dd < 4; ++dd) {
                            int x2 = cx + ((dd==0)?1:(dd==1)?-1:0);
                            int y2 = cy + ((dd==2)?1:(dd==3)?-1:0);
                            if (x2 < 0 || x2 >= W || y2 < 0 || y2 >= H) continue;
                            size_t ni = size_t(y2) * W + x2;
                            if (_objectIdBuffer[ni] == 0.f) continue;
                            float dnx = _normalBuffer[ni*3+0] - nx;
                            float dny = _normalBuffer[ni*3+1] - ny;
                            float dnz = _normalBuffer[ni*3+2] - nz;
                            sum += std::sqrt(dnx*dnx + dny*dny + dnz*dnz);
                            nc++;
                        }
                        float c = (nc > 0) ? sum / float(nc) : 0.f;
                        curvBuf[ci] = c;
                        if (c > maxCurv) maxCurv = c;
                    }
                }
                // Normalize to [0,1] so the contour-interval knob behaves
                // consistently across meshes / zoom levels.
                if (maxCurv > 1e-6f) {
                    float invMax = 1.f / maxCurv;
                    for (auto& c : curvBuf) c *= invMax;
                } else {
                    SLOG("SpectralRender: wireframe topo=curvature - mesh is effectively flat, no contours\n");
                }
            }

            if (style == 5 && (_topoDirection == 0 || _topoDirection == 1) && !hasPosition) {
                SLOG("SpectralRender: wireframe topo=height/custom needs _posBuffer - no contours rendered\n");
            }

            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    size_t i = y * W + x;
                    if (_objectIdBuffer[i] == 0.f) continue;

                    float u = _uvBuffer[i * 2 + 0];
                    float v = _uvBuffer[i * 2 + 1];

                    // Screen-space UV derivative
                    float dUdx = 0.f, dVdy = 0.f;
                    if (x + 1 < W && _objectIdBuffer[i + 1] != 0.f) {
                        dUdx = std::abs(_uvBuffer[(i + 1) * 2 + 0] - u);
                        float dvx = std::abs(_uvBuffer[(i + 1) * 2 + 1] - v);
                        dUdx = std::max(dUdx, dvx);
                    }
                    if (y + 1 < H && _objectIdBuffer[i + W] != 0.f) {
                        dVdy = std::abs(_uvBuffer[(i + W) * 2 + 1] - v);
                        float duy = std::abs(_uvBuffer[(i + W) * 2 + 0] - u);
                        dVdy = std::max(dVdy, duy);
                    }
                    float pixelUVSize = std::max(dUdx, dVdy);
                    if (pixelUVSize < 1e-8f) pixelUVSize = 1.f / float(W);

                    float lineAlpha = 0.f;
                    float lineThick = thickness;

                    // ── Style 5: Topographic contour lines ──
                    // Case 0: true height — contours of equal world Y position
                    // Case 1: arbitrary axis — contours of dot(P, customVec)
                    // Case 2: surface curvature — precomputed normalized
                    // Case 3: barycentric — UV-derived, for mesh QC
                    bool topoDataOk =
                        (_topoDirection == 0 && hasPosition) ||
                        (_topoDirection == 1 && hasPosition) ||
                        (_topoDirection == 2 && !curvBuf.empty()) ||
                        (_topoDirection == 3);
                    if (style == 5 && topoDataOk) {
                        float elevation = 0.f;
                        float dEdx = 0.f;

                        if (_topoDirection == 0) {
                            // True topographic height — world Y of the surface
                            // point, not N.y (which was slope, not height).
                            elevation = _posBuffer[i * 3 + 1];
                        } else if (_topoDirection == 1) {
                            // Axis-aligned slice: dot(P, customVec)
                            float ux = float(_topoUpVector[0]);
                            float uy = float(_topoUpVector[1]);
                            float uz = float(_topoUpVector[2]);
                            float len = std::sqrt(ux*ux + uy*uy + uz*uz);
                            if (len > 1e-6f) { ux /= len; uy /= len; uz /= len; }
                            elevation = _posBuffer[i*3+0]*ux
                                      + _posBuffer[i*3+1]*uy
                                      + _posBuffer[i*3+2]*uz;
                        } else if (_topoDirection == 2) {
                            elevation = curvBuf[i];
                            // Skip flat regions. Without this, curvature=0
                            // lands exactly on the zero-contour and every
                            // smooth pixel gets drawn (the "all white" bug).
                            if (elevation < 0.02f) continue;
                        } else /* _topoDirection == 3 */ {
                            // Barycentric — UV fractional part as elevation proxy
                            elevation = std::fmod(std::abs(u * 7.13f + v * 11.07f), 1.f);
                        }

                        // Contour frequency from interval control
                        float contourFreq = 1.f / std::max(0.01f, float(_topoContourInterval));
                        float ge = elevation * contourFreq;
                        float distE = std::abs(ge - std::round(ge)) / contourFreq;

                        // Screen-space derivative of elevation
                        if (_topoDirection != 3) {
                            auto getElev = [&](size_t idx) -> float {
                                if (_topoDirection == 0) return _posBuffer[idx*3+1];
                                if (_topoDirection == 1) {
                                    float ux=float(_topoUpVector[0]), uy=float(_topoUpVector[1]), uz=float(_topoUpVector[2]);
                                    float len=std::sqrt(ux*ux+uy*uy+uz*uz);
                                    if(len>1e-6f){ux/=len;uy/=len;uz/=len;}
                                    return _posBuffer[idx*3]*ux
                                         + _posBuffer[idx*3+1]*uy
                                         + _posBuffer[idx*3+2]*uz;
                                }
                                if (_topoDirection == 2) return curvBuf[idx];
                                return 0.f;
                            };
                            if (x+1 < W && _objectIdBuffer[i+1] != 0.f)
                                dEdx = std::abs(getElev(i+1) - elevation);
                            if (y+1 < H && _objectIdBuffer[i+W] != 0.f)
                                dEdx = std::max(dEdx, std::abs(getElev(i+W) - elevation));
                        } else {
                            // Barycentric: use UV derivative
                            dEdx = pixelUVSize * 10.f;
                        }
                        if (dEdx < 1e-8f) dEdx = 0.01f;

                        float pixDistE = distE / dEdx;

                        // Major contours
                        int majorEvery = std::max(2, _topoMajorEvery);
                        float majorFrac = 1.f / float(majorEvery);
                        float majorDist = std::abs(ge * majorFrac - std::round(ge * majorFrac));
                        bool isMajor = (majorDist < 0.15f);
                        lineThick = isMajor ? thickness * 2.5f : thickness;

                        if (pixDistE < lineThick + aaBand) {
                            float cov = coverage(pixDistE, lineThick);
                            if (cov > 0.f) {
                                lineAlpha = opacity * cov;
                                if (!isMajor) lineAlpha *= 0.5f;
                            }
                        }
                    } else {

                    // ── UV-grid based styles (0-4) ──
                    {
                        float gu = u * gridFreq;
                        float gv = v * gridFreq;
                        float distU = std::abs(gu - std::round(gu)) / gridFreq;
                        float distV = std::abs(gv - std::round(gv)) / gridFreq;
                        float edgeDist = std::min(distU, distV);
                        float pixelDist = edgeDist / pixelUVSize;

                        // ── Style 0: Solid ──
                        if (style == 0) {
                            float cov = coverage(pixelDist, lineThick);
                            if (cov <= 0.f) continue;
                            lineAlpha = opacity * cov;
                        }

                        // ── Style 1: Guide (thin dashed construction lines) ──
                        else if (style == 1) {
                            lineThick *= 0.5f;
                            float cov = coverage(pixelDist, lineThick);
                            if (cov <= 0.f) continue;
                            lineAlpha = opacity * 0.6f * cov;
                            // Dash pattern
                            float dashPos = std::fmod(float(x + y) * 0.7071f,
                                                     float(_wireDashLength) + float(_wireGapLength));
                            if (dashPos > float(_wireDashLength)) continue;
                        }

                        // ── Style 2: Architectural blueprint ──
                        else if (style == 2) {
                            float majorDistU = std::abs(std::round(u) - u);
                            float majorDistV = std::abs(std::round(v) - v);
                            float majorPixU = majorDistU / pixelUVSize;
                            float majorPixV = majorDistV / pixelUVSize;
                            bool isMajor = (majorPixU < 3.f) || (majorPixV < 3.f);

                            float med5U = std::abs(u * gridFreq * 0.2f - std::round(u * gridFreq * 0.2f)) / (gridFreq * 0.2f);
                            float med5V = std::abs(v * gridFreq * 0.2f - std::round(v * gridFreq * 0.2f)) / (gridFreq * 0.2f);
                            float medDist = std::min(med5U, med5V) / pixelUVSize;
                            bool isMedium = (medDist < 2.f);

                            if (isMajor) {
                                lineThick *= float(_archSilhouetteWeight);
                            } else if (isMedium) {
                                lineThick *= float(_archMediumWeight);
                            } else {
                                lineThick *= float(_archThinWeight);
                            }

                            float cov = coverage(pixelDist, lineThick);
                            if (cov <= 0.f) continue;
                            lineAlpha = opacity * cov;

                            // Line hierarchy opacity
                            if (!isMajor && !isMedium) lineAlpha *= float(_archThinOpacity);
                            else if (isMedium) lineAlpha *= 0.7f;

                            // Silhouette color override
                            if (isMajor) {
                                float* px = _frameBuffer.data() + i * 4;
                                float invA = 1.f - lineAlpha;
                                px[0] = px[0] * invA + float(_archSilhouetteColor[0]) * lineAlpha;
                                px[1] = px[1] * invA + float(_archSilhouetteColor[1]) * lineAlpha;
                                px[2] = px[2] * invA + float(_archSilhouetteColor[2]) * lineAlpha;
                                px[3] = std::min(1.f, px[3] + lineAlpha);
                                lineAlpha = 0.f; // already blended
                            }
                        }

                        // ── Style 3: Hidden-line ──
                        else if (style == 3) {
                            float cov = coverage(pixelDist, lineThick);
                            if (cov <= 0.f) continue;
                            lineAlpha = opacity * cov;
                            // Secondary direction dimmed and dashed
                            if (distU < distV) {
                                lineAlpha *= 0.3f;
                                float dashPos = std::fmod(float(x) * 0.5f, 6.f);
                                if (dashPos > 3.f) continue;
                            }
                        }

                        // ── Style 4: Pencil sketch ──
                        else if (style == 4) {
                            float wobbleAmt = float(_pencilWobble);
                            float pressureAmt = float(_pencilPressure);

                            // UV-space noise cells: the pattern is keyed off
                            // the mesh surface, not screen pixels, so it
                            // follows the geometry under animation / camera
                            // moves. Cell size ~1 pixel at current zoom.
                            const float invUVSize = 1.f / pixelUVSize;
                            int cellU = int(u * invUVSize);
                            int cellV = int(v * invUVSize);

                            // Wobble: offset the grid distance with noise
                            float noise = pencilNoise(cellU, cellV);
                            float wobble = (noise - 0.5f) * wobbleAmt * pixelUVSize;
                            float distUw = std::abs((gu + wobble * gridFreq) - std::round(gu + wobble * gridFreq)) / gridFreq;
                            float distVw = std::abs((gv + wobble * gridFreq) - std::round(gv + wobble * gridFreq)) / gridFreq;
                            float edgeDistW = std::min(distUw, distVw);
                            float pixDistW = edgeDistW / pixelUVSize;

                            // Variable thickness from pressure — lower-freq
                            // noise (every ~4 pixels worth of UV).
                            int cellU4 = int(u * invUVSize * 0.25f);
                            int cellV4 = int(v * invUVSize * 0.25f);
                            float thickNoise = pencilNoise(cellU4, cellV4);
                            float pencilThick = lineThick * (1.f - pressureAmt + thickNoise * pressureAmt * 2.f);

                            float cov = coverage(pixDistW, pencilThick);
                            if (cov <= 0.f) continue;
                            lineAlpha = opacity * (1.f - pressureAmt * 0.5f + thickNoise * pressureAmt * 0.5f) * cov;

                            // Cross-hatch in shadowed areas — UV-space too,
                            // so the hatch pattern rides the surface.
                            if (_pencilCrossHatch && hasNormals) {
                                float ny = _normalBuffer[i * 3 + 1];
                                if (ny < 0.3f) {
                                    // Hatch spacing scaled from screen pixels
                                    // into UV units at current zoom, so the
                                    // knob remains "pixels between lines".
                                    float hatchSpacing = float(_pencilHatchDensity) * pixelUVSize;
                                    float angleRad = float(_pencilHatchAngle) * float(M_PI) / 180.f;
                                    float hatchCoord = u * std::cos(angleRad) + v * std::sin(angleRad);
                                    float hatch = std::fmod(std::abs(hatchCoord), hatchSpacing);
                                    if (hatch < hatchSpacing * 0.3f) {
                                        float hatchAlpha = opacity * 0.3f * (0.3f - ny);
                                        // Wobble the hatch lines too — also UV-keyed.
                                        hatchAlpha *= (0.7f + pencilNoise(cellU + 999, cellV + 999) * 0.6f);
                                        lineAlpha = std::max(lineAlpha, hatchAlpha);
                                    }
                                }
                            }
                        }
                    }

                    // Dashed override (applies to solid style when checkbox is on)
                    if (_wireDashed && (style == 0 || style == 3)) {
                        float dashPos = std::fmod(float(x + y) * 0.7071f,
                                                 float(_wireDashLength) + float(_wireGapLength));
                        if (dashPos > float(_wireDashLength)) continue;
                    }

                    } // end else (non-topo styles)

                    if (lineAlpha > 0.001f) {
                        float* px = _frameBuffer.data() + i * 4;
                        float invA = 1.f - lineAlpha;
                        px[0] = px[0] * invA + wireR * lineAlpha;
                        px[1] = px[1] * invA + wireG * lineAlpha;
                        px[2] = px[2] * invA + wireB * lineAlpha;
                        px[3] = std::min(1.f, px[3] + lineAlpha);
                    }
                }
            }
            fprintf(stderr, "SpectralRender: wireframe overlay complete (%dx%d, style=%d)\n", W, H, style);
        }

        // Cryptomatte
        if (!_cryptoObjectBuffer.empty() && !_objectIdBuffer.empty()) {
            for (size_t i = 0; i < size_t(W) * H; ++i) {
                float objId = _objectIdBuffer[i];
                uint32_t h = static_cast<uint32_t>(objId);
                h ^= h >> 16; h *= 0x85ebca6b;
                h ^= h >> 13; h *= 0xc2b2ae35;
                h ^= h >> 16;
                h &= 0x7fffffff; if ((h & 0x7f800000) == 0x7f800000) h = 0;
                float hashF;
                memcpy(&hashF, &h, sizeof(float));
                _cryptoObjectBuffer[i * 4 + 0] = hashF;
                _cryptoObjectBuffer[i * 4 + 1] = (objId > 0.f) ? 1.f : 0.f;
                _cryptoObjectBuffer[i * 4 + 2] = 0.f;
                _cryptoObjectBuffer[i * 4 + 3] = 0.f;
            }
        }

        _progressiveSppDone = _renderSpp;

        // Mark all strips rendered
        for (size_t i = 0; i < _stripRendered.size(); ++i)
            _stripRendered[i] = true;

        auto tEnd = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
        SLOG("SpectralRender: render complete (%lldms)\n", ms);
    }

    _stripRendered[stripIdx] = true;
}

// ---------------------------------------------------------------------------
// DeepOp interface
// ---------------------------------------------------------------------------
void SpectralRenderIop::getDeepRequests(DD::Image::Box /*box*/,
                                         const DD::Image::ChannelSet& /*channels*/,
                                         int /*count*/,
                                         std::vector<RequestData>& /*reqData*/)
{
    // No deep inputs to request from — we generate all data ourselves.
}

bool SpectralRenderIop::doDeepEngine(DD::Image::Box box,
                                      const DD::Image::ChannelSet& channels,
                                      DeepOutputPlane& plane)
{
    _EnsureSceneReady();
    if (!_stripRendered.empty() && !_stripRendered[0])
        _RenderStrip(0);

    const int W = static_cast<int>(_fbWidth);
    const int H = static_cast<int>(_fbHeight);

    if (_frameBuffer.empty() || _depthBuffer.empty() || W == 0 || H == 0)
        return true;

    // Proxy scaling
    const Format* fmt = &(info_.format());
    const int fmtW = fmt ? fmt->width() : W;
    const int fmtH = fmt ? fmt->height() : H;
    const float scaleX = float(W) / float(std::max(1, fmtW));
    const float scaleY = float(H) / float(std::max(1, fmtH));

    DD::Image::ChannelSet outChans = channels;
    outChans += Chan_DeepFront;
    outChans += Chan_DeepBack;
    plane = DeepOutputPlane(outChans, box, DeepPixel::eZAscending);
    const int nChans = outChans.size();

    // Deep samples per pixel (0 = disabled, use shell fallback)
    const int deepN = std::max(0, _deepSamples);
    const bool hasVolumes = !_volumes.empty();

    // Camera matrices for ray generation and depth conversion
    const pxr::GfMatrix4d& v2w = _camera.viewToWorld;
    const pxr::GfMatrix4d& pInv = _camera.projInverse;
    const pxr::GfMatrix4d w2v = v2w.GetInverse();
    const double imgW = double(_camera.imageWidth);
    const double imgH = double(_camera.imageHeight);
    const double pxAspect = _camera.pixelAspect;

    for (DD::Image::Box::iterator it = box.begin(); it != box.end(); ++it) {
        const int px = it.x;
        const int py = it.y;
        const int bufX = int(px * scaleX);
        const int bufY = H - 1 - int(py * scaleY);

        if (bufX < 0 || bufX >= W || bufY < 0 || bufY >= H) {
            plane.addHole(); continue;
        }

        const size_t pixIdx = static_cast<size_t>(bufY) * W + bufX;
        const float* rgba = _frameBuffer.data() + pixIdx * 4;
        const float surfDepth = _depthBuffer[pixIdx];

        // ---- Volumetric deep: march volumes and output per-slab samples ----
        if (hasVolumes && deepN > 0) {
            // Skip pixels with no content
            if (rgba[3] < 0.0001f) {
                plane.addHole();
                continue;
            }

            // Generate camera ray for this pixel
            double ndcX = (2.0 * (bufX + 0.5) / imgW - 1.0) / pxAspect;
            double ndcY = 1.0 - 2.0 * (bufY + 0.5) / imgH;
            pxr::GfVec3d viewNear = pInv.Transform(pxr::GfVec3d(ndcX, ndcY, -1.0));
            pxr::GfVec3d viewFar  = pInv.Transform(pxr::GfVec3d(ndcX, ndcY,  1.0));
            pxr::GfVec3d worldNear = v2w.Transform(viewNear);
            pxr::GfVec3d worldFar  = v2w.Transform(viewFar);
            pxr::GfVec3f ro(worldNear);
            pxr::GfVec3f rd(pxr::GfVec3f(worldFar - worldNear).GetNormalized());

            // ---- Pass 1: march volumes, collect slab weights ----
            struct DeepSlab { float front; float back; float weight; float alpha; };
            std::vector<DeepSlab> slabs;
            float totalWeight = 0.f;

            for (auto& vol : _volumes) {
                if (!vol || !vol->IsValid()) continue;

                pxr::GfVec3f invDir(
                    1.f/(std::abs(rd[0])>1e-8f?rd[0]:1e-8f),
                    1.f/(std::abs(rd[1])>1e-8f?rd[1]:1e-8f),
                    1.f/(std::abs(rd[2])>1e-8f?rd[2]:1e-8f));
                pxr::GfVec3f t0v(
                    (vol->GetBboxMin()[0]-ro[0])*invDir[0],
                    (vol->GetBboxMin()[1]-ro[1])*invDir[1],
                    (vol->GetBboxMin()[2]-ro[2])*invDir[2]);
                pxr::GfVec3f t1v(
                    (vol->GetBboxMax()[0]-ro[0])*invDir[0],
                    (vol->GetBboxMax()[1]-ro[1])*invDir[1],
                    (vol->GetBboxMax()[2]-ro[2])*invDir[2]);
                float tNear = std::max({std::min(t0v[0],t1v[0]),std::min(t0v[1],t1v[1]),std::min(t0v[2],t1v[2])});
                float tFar  = std::min({std::max(t0v[0],t1v[0]),std::max(t0v[1],t1v[1]),std::max(t0v[2],t1v[2])});
                tNear = std::max(tNear, 0.f);
                if (tNear >= tFar) continue;

                // Adaptive step at voxel resolution
                pxr::GfVec3f bboxSize = vol->GetBboxMax() - vol->GetBboxMin();
                float voxelSize = std::max({bboxSize[0]/std::max(1.f,float(vol->resX)),
                                            bboxSize[1]/std::max(1.f,float(vol->resY)),
                                            bboxSize[2]/std::max(1.f,float(vol->resZ))});
                float quality = std::max(0.25f, float(deepN) / 32.f);
                float dt = voxelSize / quality;
                float transmittance = 1.f;
                int maxSteps = std::min(512, int((tFar - tNear) / dt) + 1);

                float t = tNear;
                for (int step = 0; step < maxSteps && t < tFar; ++step) {
                    float tFront = t;
                    float tBack  = std::min(t + dt, tFar);
                    float tMid   = (tFront + tBack) * 0.5f;

                    pxr::GfVec3f p = ro + rd * tMid;
                    pxr::GfVec3f uv = vol->WorldToNorm(p);
                    float density = 0.f;
                    if (uv[0]>=0&&uv[0]<=1&&uv[1]>=0&&uv[1]<=1&&uv[2]>=0&&uv[2]<=1)
                        density = vol->SampleDensity(uv[0], uv[1], uv[2]) * vol->densityMult;

                    if (density < 1e-5f) {
                        t += dt * 4.f;
                        continue;
                    }

                    float stepDt = tBack - tFront;
                    float sigma_t = density * vol->extinction;
                    float slabTrans = std::exp(-sigma_t * stepDt);
                    float slabAlpha = 1.f - slabTrans;

                    if (slabAlpha < 0.0001f) { t += dt; continue; }

                    // Weight = contribution of this slab to final pixel
                    float w = transmittance * slabAlpha;

                    // Convert to camera-Z
                    pxr::GfVec3d wFront = pxr::GfVec3d(ro) + pxr::GfVec3d(rd) * double(tFront);
                    pxr::GfVec3d wBack  = pxr::GfVec3d(ro) + pxr::GfVec3d(rd) * double(tBack);
                    float zFront = float(-w2v.Transform(wFront)[2]);
                    float zBack  = float(-w2v.Transform(wBack)[2]);

                    if (zFront > 0.f && zBack > 0.f) {
                        slabs.push_back({zFront, zBack, w, slabAlpha});
                        totalWeight += w;
                    }

                    transmittance *= slabTrans;
                    if (transmittance < 0.001f) break;
                    t += dt;
                }
            }

            if (slabs.empty()) {
                plane.addHole();
                continue;
            }

            // ---- Pass 2: distribute beauty RGBA across slabs by weight ----
            // This guarantees DeepToImage reconstructs the beauty exactly.
            std::sort(slabs.begin(), slabs.end(),
                [](const DeepSlab& a, const DeepSlab& b) { return a.front < b.front; });

            float invTotal = (totalWeight > 1e-6f) ? 1.f / totalWeight : 0.f;

            DeepOutPixel op(nChans * slabs.size());
            for (auto& slab : slabs) {
                float frac = slab.weight * invTotal;
                // Premultiplied: RGB scaled by fraction of beauty, alpha = slab opacity
                float sr = rgba[0] * frac;
                float sg = rgba[1] * frac;
                float sb = rgba[2] * frac;
                float sa = rgba[3] * frac;

                foreach(z, outChans) {
                    if      (z == Chan_DeepFront) op.push_back(slab.front);
                    else if (z == Chan_DeepBack)  op.push_back(slab.back);
                    else if (z == Chan_Red)       op.push_back(sr);
                    else if (z == Chan_Green)     op.push_back(sg);
                    else if (z == Chan_Blue)      op.push_back(sb);
                    else if (z == Chan_Alpha)     op.push_back(sa);
                    else                          op.push_back(0.f);
                }
            }
            plane.addPixel(op);
            continue;
        }

        // ---- Fallback: single shell sample (no volumes or deepSamples=0) ----
        if (surfDepth >= 1e29f) {
            plane.addHole();
            continue;
        }

        DeepOutPixel op(nChans);
        foreach(z, outChans) {
            if      (z == Chan_DeepFront) op.push_back(surfDepth);
            else if (z == Chan_DeepBack)  op.push_back(surfDepth + 0.001f);
            else if (z == Chan_Red)       op.push_back(rgba[0]);
            else if (z == Chan_Green)     op.push_back(rgba[1]);
            else if (z == Chan_Blue)      op.push_back(rgba[2]);
            else if (z == Chan_Alpha)     op.push_back(rgba[3]);
            else                          op.push_back(0.f);
        }
        plane.addPixel(op);
    }

    return true;
}
