#include "SpectralRenderIop.h"
#include "SpectralSurfaceOp.h"

#ifdef SPECTRAL_HAS_OSD
#include "SpectralSubdiv.h"
#endif

#ifdef SPECTRAL_HAS_VDB
#include "SpectralVDBLoader.h"
#include "SpectralVDBRead.h"
#include "SpectralVolumeMaterial.h"
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
    // 5 inputs: scene, cam, bg, vol, vol_mat (set by min/max_inputs)
    fprintf(stderr, "SpectralRender: DLL build %s %s\n", __DATE__, __TIME__);
}

SpectralRenderIop::~SpectralRenderIop() = default;

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

const char* SpectralRenderIop::node_help() const
{
    return
        "SpectralRender — physically-based spectral path tracer.\n\n"
        "Input 0 (Scene): Connect any USD GeomOp\n"
        "(GeoCube, GeoSphere, GeoImport, etc.)\n\n"
        "Input 1 (Cam): Connect a Camera node.\n"
        "If not connected, uses first camera found in the USD stage,\n"
        "or a default 50mm perspective.\n\n"
        "Input 2 (BG): Connect any 2D image. Its format sets the\n"
        "output resolution. The BG image is rendered behind the scene.\n\n"
        "Input 3 (Vol): Connect a SpectralVDBRead node.\n"
        "Volume shading controls are on the Volumes tab.\n\n"
        "Created by Marten Blumen\n"
        "github.com/bratgot/SpectralRenderer";}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
const char* SpectralRenderIop::input_label(int idx, char*) const
{
    switch (idx) {
        case 0: return "scn";
        case 1: return "cam";
        case 2: return "bg";
        case 3: return "vol";
        case 4: return "mat";
        default: return nullptr;
    }
}

bool SpectralRenderIop::test_input(int idx, Op* op) const
{
    if (!op) return true;  // allow disconnection on any input

    switch (idx) {
        case 0: {
            // Accept GeometryProviderI (GeoScene, USD scene nodes)
            return dynamic_cast<GeometryProviderI*>(op) != nullptr;
        }
        case 1: {
            return dynamic_cast<CameraOp*>(op) != nullptr;
        }
        case 2: {
            return dynamic_cast<Iop*>(op) != nullptr;
        }
        case 3: {
            // Vol: accept any Op (SpectralVDBRead is a SourceGeomOp)
            return true;
        }
        case 4: {
            // VolMat: accept SpectralVolumeMaterial (ShaderOp)
            return true;
        }
        default:
            return false;
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

    Divider(f, "Scene");
    File_knob(f, &_usdFilePath, "usd_file", "USD file");
    Tooltip(f, "Path to a .usd/.usda/.usdc file.<br>"
               "Only used when the Scene input is not connected.");
    String_knob(f, &_cameraPath, "camera_path", "camera prim");
    Tooltip(f, "USD prim path to a specific camera,<br>"
               "e.g. /World/Camera1. Leave blank to<br>"
               "auto-detect the first camera in the stage.");
    Format_knob(f, &_outputFormat, "format", "format");
    Tooltip(f, "Output image resolution. If a background<br>"
               "input is connected, its format is used instead.");

    BeginGroup(f, "render_grp", "Render");
    {
        static const char* const deviceNames[] = { "cpu", "gpu", "auto", nullptr };
        Enumeration_knob(f, &_deviceMode, deviceNames, "device_mode", "device mode");
        Tooltip(f, "Rendering device:<br>"
                   "cpu = Embree 4 CPU ray tracing<br>"
                   "gpu = OptiX 8.1 RTX GPU ray tracing<br>"
                   "auto = GPU if available, otherwise CPU");
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
        Int_knob(f, &_samples, "spp", "samples"); SetRange(f, 1, 256);
        Tooltip(f, "Number of spectral samples per pixel.<br>"
                   "Higher values reduce noise. Each sample<br>"
                   "traces one wavelength through the scene.<br>"
                   "1 = normal-shaded preview, 16+ = spectral.");
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
        Divider(f, "Motion blur");
        Float_knob(f, &_shutterOpen, "shutter_open", "shutter open"); SetRange(f, -1.f, 0.f);
        Tooltip(f, "Shutter open time relative to frame.<br>"
                   "-0.5 = half frame before (centred shutter)<br>"
                   "0.0 = starts at current frame");
        Float_knob(f, &_shutterClose, "shutter_close", "shutter close"); SetRange(f, 0.f, 1.f);
        Tooltip(f, "Shutter close time relative to frame.<br>"
                   "0.5 = half frame after (180 degree shutter)<br>"
                   "0.0 = no motion blur (both open and close at 0)");
    }
    EndGroup(f);

    Tab_knob(f, "Lighting");
    {
        static const char* const skyP[] = {"Off", "Custom", "Day Sky", "Golden Hour", "Overcast", "Blue Hour", "Night", nullptr};
        Enumeration_knob(f, &_skyPreset, skyP, "sky_preset", "Sky Preset");
        Tooltip(f, "Analytical sun/sky lighting (Preetham model).\n"
                   "Composes with scene lights from the scn input.\n"
                   "Off = disabled, Day Sky = noon clear,\n"
                   "Golden Hour = warm sunset, Overcast = soft cloudy,\n"
                   "Blue Hour = cool twilight, Night = moonlit.");
        Double_knob(f, &_skyMix, "sky_mix", "sky mix"); SetRange(f, 0, 2);

        BeginClosedGroup(f, "grp_sun", "Sun and sky settings");
        {
            Double_knob(f, &_sunElevation, "sun_elevation", "sun elevation"); SetRange(f, 0, 90);
            Tooltip(f, "Sun angle above horizon. 90=noon, 10=sunset.");
            Double_knob(f, &_sunAzimuth, "sun_azimuth", "sun azimuth"); SetRange(f, 0, 360);
            Double_knob(f, &_sunIntensity, "sun_intensity", "sun intensity"); SetRange(f, 0, 20);
            Double_knob(f, &_skyIntensity, "sky_intensity", "sky fill"); SetRange(f, 0, 5);
            Double_knob(f, &_turbidity, "turbidity", "turbidity"); SetRange(f, 2, 10);
            Tooltip(f, "Atmospheric haze. 2=clear, 5=hazy, 10=heavy smog.");
        }
        EndGroup(f);

        Divider(f, "Studio lights");
        static const char* const stuP[] = {"Off", "Portrait", "Product", "Dramatic", "Softbox", nullptr};
        Enumeration_knob(f, &_studioPreset, stuP, "studio_preset", "Studio Preset");
        Tooltip(f, "Three-point studio light rig.\n"
                   "Portrait = soft key/fill, subtle rim.\n"
                   "Product = bright key, strong rim.\n"
                   "Dramatic = hard single key, dark fill.\n"
                   "Softbox = broad soft key, gentle fill.");
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

        Divider(f, "Shadow softness");
        Double_knob(f, &_shadowSoftness, "shadow_softness", "shadow softness"); SetRange(f, 0, 1);
        Tooltip(f, "Softens shadows from built-in lights.\n"
                   "0 = hard shadows (point source)\n"
                   "0.3 = soft (default for studio)\n"
                   "1.0 = very diffuse (overcast look)\n"
                   "Simulates area light sources.");
    }

    Tab_knob(f, "Volumes");
    {
        Text_knob(f,
            "<font color='#888' size='-1'>"
            "Load OpenVDB files for volume rendering. "
            "Volumes composite over scene geometry using spectral ray marching."
            "</font>"
        );
        File_knob(f, &_vdbFile, "vdb_file", "VDB file");
        Tooltip(f, "OpenVDB volume file (.vdb).\n"
                   "Supports #### frame padding for sequences.");
        Bool_knob(f, &_vdbAutoSequence, "vdb_auto_sequence", "auto sequence");
        ClearFlags(f, Knob::STARTLINE);
        Int_knob(f, &_vdbFrameOffset, "vdb_frame_offset", "frame offset");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, -100, 100);
        Obsolete_knob(f, "vdb_orig_file", nullptr);
        String_knob(f, &_vdbOrigFile, "vdb_orig_file", "");
        SetFlags(f, Knob::INVISIBLE);

        Divider(f, "Viewport preview");
        Bool_knob(f, &_vdbShowBbox, "vdb_show_bbox", "bbox");
        Tooltip(f, "Green wireframe bounding box around the volume.");
        Bool_knob(f, &_vdbShowPoints, "vdb_show_points", "points");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Point cloud preview — denser regions appear\n"
                   "brighter with a heat-map colour ramp.");
        Double_knob(f, &_vdbPointDensity, "vdb_point_density", "density");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 0.1, 1.0);
        Tooltip(f, "Proportion of voxels to sample as points.\n"
                   "0.1 = sparse/fast, 1.0 = every voxel.");
        Double_knob(f, &_vdbPointSize, "vdb_point_size", "size");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 1, 8);
        Tooltip(f, "GL point size in pixels.");

        Newline(f);
        Bool_knob(f, &_vdbFastScrub, "vdb_fast_scrub", "fast scrub");
        Tooltip(f, "Bbox-only during timeline scrub (~1ms per frame).\n"
                   "Full point cloud loads when playback stops.\n"
                   "Disable for real-time point cloud updates.");
        Bool_knob(f, &_vdbCacheEnabled, "vdb_cache_enabled", "cache");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Cache recently visited VDB frames in memory.\n"
                   "Scrubbing back to a cached frame is instant.");
        Int_knob(f, &_vdbCacheMax, "vdb_cache_max", "");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 1, 32);
        Tooltip(f, "Number of VDB frames to keep in memory.\n"
                   "Each preview frame ≈ 1 MB, render frame ≈ 8 MB.");
        Text_knob(f, "frames");
        ClearFlags(f, Knob::STARTLINE);
        Button(f, "vdb_cache_clear", "Clear Cache");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Free all cached VDB frames from memory.");

        Divider(f, "Grids");
        Button(f, "discover_grids", "Discover Grids");
        Tooltip(f, "Scan VDB file and auto-detect grid names.");
        Enumeration_knob(f, &_vdbDensityGridIdx, kVdbGridMenu, "vdb_density_grid", "density");
        String_knob(f, &_vdbDensityOverride, "vdb_density_override", "override");
        ClearFlags(f, Knob::STARTLINE);
        Enumeration_knob(f, &_vdbTempGridIdx, kVdbGridMenu, "vdb_temp_grid", "temperature");
        String_knob(f, &_vdbTempOverride, "vdb_temp_override", "override");
        ClearFlags(f, Knob::STARTLINE);
        Enumeration_knob(f, &_vdbFlameGridIdx, kVdbGridMenu, "vdb_flame_grid", "flame");
        String_knob(f, &_vdbFlameOverride, "vdb_flame_override", "override");
        ClearFlags(f, Knob::STARTLINE);
        Enumeration_knob(f, &_vdbColorGridIdx, kVdbGridMenu, "vdb_color_grid", "color");
        String_knob(f, &_vdbColorOverride, "vdb_color_override", "override");
        ClearFlags(f, Knob::STARTLINE);

        Divider(f, "Shading Preset");
        static const char* const volPresets[] = {
            "Custom", "Smoke", "Fire / Explosion", "Clouds", "Fog / Mist", "Nebula", nullptr
        };
        Enumeration_knob(f, &_vdbShadingPreset, volPresets, "vdb_shading_preset", "Shading Preset");
        Double_knob(f, &_vdbExtinction, "vdb_extinction", "extinction"); SetRange(f, 0, 20);
        Tooltip(f, "Extinction coefficient (sigma_t).\n"
                   "Controls how quickly light is absorbed.\n"
                   "0 = fully transparent, 5 = default smoke,\n"
                   "10-20 = very dense/opaque.");
        Double_knob(f, &_vdbScattering, "vdb_scattering", "scattering"); SetRange(f, 0, 20);
        Tooltip(f, "In-scatter coefficient (sigma_s).\n"
                   "How much light scatters into the camera.\n"
                   "Higher = brighter volume interior.");
        Double_knob(f, &_vdbDensityMult, "vdb_density_mult", "density multiplier"); SetRange(f, 0, 10);
        Tooltip(f, "Scales the raw density values from the VDB.\n"
                   "1 = use as-is, 2 = double density,\n"
                   "0.5 = half density. Useful for simulations\n"
                   "with very high or low density ranges.");
        Double_knob(f, &_vdbAnisotropy, "vdb_anisotropy", "anisotropy"); SetRange(f, -1, 1);
        Tooltip(f, "Henyey-Greenstein phase function g parameter.\n"
                   " 0.0 = isotropic (equal in all directions)\n"
                   " 0.6 = forward scatter (clouds, fog)\n"
                   " 0.8 = strong forward (mist, haze)\n"
                   "-0.3 = backward scatter (silver lining effect)");
        Double_knob(f, &_vdbStepSize, "vdb_step_size", "step size"); SetRange(f, 0, 2);
        Tooltip(f, "Ray march step size in world units.\n"
                   "0 = automatic (half voxel size).\n"
                   "Smaller = more accurate but slower.\n"
                   "Larger = faster but may miss thin features.");
        Int_knob(f, &_vdbMaxSteps, "vdb_max_steps", "max steps"); SetRange(f, 64, 1024);
        Tooltip(f, "Maximum ray march steps per ray.\n"
                   "256 = default. Increase for very large or\n"
                   "very dense volumes. Decrease for faster preview.");

        Divider(f, "Emission");
        Double_knob(f, &_vdbEmissionIntensity, "vdb_emission_intensity", "emission intensity"); SetRange(f, 0, 20);
        Tooltip(f, "Brightness of blackbody emission.\n"
                   "0 = no emission, 2 = default fire,\n"
                   "5-10 = bright explosion.");
        Double_knob(f, &_vdbTempMin, "vdb_temp_min", "temp min"); SetRange(f, 0, 5000);
        Tooltip(f, "Temperature at which emission begins (Kelvin).\n"
                   "500 = dull red, 1000 = orange, 2000 = yellow.");
        Double_knob(f, &_vdbTempMax, "vdb_temp_max", "temp max"); SetRange(f, 500, 40000);
        Tooltip(f, "Temperature at full emission brightness.\n"
                   "6500 = daylight white, 10000+ = blue-white.\n"
                   "Range maps temperature grid to 0-1 emission.");
        Double_knob(f, &_vdbFlameIntensity, "vdb_flame_intensity", "flame intensity"); SetRange(f, 0, 20);
        Tooltip(f, "Brightness multiplier for the flame grid.\n"
                   "Separate from temperature-based emission.\n"
                   "5 = default. Set to 0 to disable flame.");

        BeginClosedGroup(f, "vdb_phase", "Phase function (advanced)");
        {
            Double_knob(f, &_vdbGForward, "vdb_g_forward", "forward lobe"); SetRange(f, 0, 1);
            Tooltip(f, "Forward scattering lobe strength.\n"
                       "0 = isotropic, 0.65 = clouds (default),\n"
                       "0.9 = strong forward (thin mist).");
            Double_knob(f, &_vdbGBackward, "vdb_g_backward", "backward lobe"); SetRange(f, -1, 0);
            Tooltip(f, "Backward scattering lobe strength.\n"
                       "0 = none, -0.25 = default silver lining,\n"
                       "-0.5 = strong back-scatter.");
            Double_knob(f, &_vdbLobeMix, "vdb_lobe_mix", "lobe mix"); SetRange(f, 0, 1);
            Tooltip(f, "Balance between forward and backward lobes.\n"
                       "1.0 = pure forward, 0.0 = pure backward,\n"
                       "0.7 = natural cloud mix (default).");
            Double_knob(f, &_vdbPowder, "vdb_powder", "powder effect"); SetRange(f, 0, 10);
            Tooltip(f, "Interior brightening (Schneider & Vos 2015).\n"
                       "0=off, 2=natural, 6=dense fireball.");
            Bool_knob(f, &_vdbJitter, "vdb_jitter", "jitter steps");
            Tooltip(f, "Stochastic step offset to remove banding.");
        }
        EndGroup(f);

        BeginClosedGroup(f, "vdb_scatter", "Scatter colour");
        {
            Color_knob(f, _vdbScatterColor, "vdb_scatter_color", "scatter color");
            Tooltip(f, "Tint of scattered light inside the volume.\n"
                       "White=neutral, warm=fire, blue=atmosphere.");
        }
        EndGroup(f);

        Divider(f, "Quality");
        Text_knob(f,
            "<font color='#666' size='-1'>"
            "Shadow rays and step quality control render fidelity.<br>"
            "Start with lower quality for layout, increase for finals."
            "</font>"
        );
        Newline(f);
        Double_knob(f, &_vdbQuality, "vdb_quality", "quality");
        SetRange(f, 1, 10);
        Tooltip(f, "Ray march step resolution (logarithmic).\n"
                   "Step size = 1/(quality^2).\n"
                   "1 = fast preview, 5 = good, 7 = high, 10 = final.");
        Bool_knob(f, &_vdbAdaptiveStep, "vdb_adaptive_step", "adaptive");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Larger steps in thin regions, smaller in dense.\n"
                   "2-4x speedup with minimal quality loss.");
        Int_knob(f, &_vdbShadowSteps, "vdb_shadow_steps", "shadow steps");
        SetRange(f, 0, 64);
        Tooltip(f, "Shadow ray samples per light per march step.\n"
                   "0 = no self-shadowing (flat look).\n"
                   "4-8 = preview, 16-32 = final render.");
        Double_knob(f, &_vdbShadowDensity, "vdb_shadow_density", "shadow density");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 0, 5);
        Tooltip(f, "Extinction multiplier for shadow rays.\n"
                   "1 = physical, <1 = lighter shadows, >1 = darker.\n"
                   "0 = no self-shadowing.");

        BeginClosedGroup(f, "vdb_ms", "Multiple scatter");
        {
            Bool_knob(f, &_vdbMsApprox, "vdb_ms_approx", "analytical MS");
            Tooltip(f, "Wrenninge 2015 analytical multiple scattering.\n"
                       "Approximates infinite-bounce light at near-zero cost.\n"
                       "Brightens dense volume interiors realistically.");
            Color_knob(f, _vdbMsTint, "vdb_ms_tint", "tint");
            Tooltip(f, "Colour tint on the multi-scatter contribution.\n"
                       "Default (1.0, 0.97, 0.95) = slight warm bias.");
        }
        EndGroup(f);
    }

    Tab_knob(f, "AOVs");

    BeginClosedGroup(f, "aov_grp", "AOV outputs");
    {
        Divider(f, "Geometry");
        Bool_knob(f, &_aovNormals, "aov_normals", "N");
        Bool_knob(f, &_aovPosition, "aov_position", "P");
        Bool_knob(f, &_aovPRef, "aov_pref", "Pref");
        Bool_knob(f, &_aovUV, "aov_uv", "UV");
        Bool_knob(f, &_aovAlbedo, "aov_albedo", "albedo");
        Tooltip(f, "N = world normals, P = world position,<br>"
                   "Pref = undisplaced object-space position,<br>"
                   "UV = texture coordinates, albedo = surface colour.");

        Divider(f, "Lighting");
        Bool_knob(f, &_aovDirect, "aov_direct", "direct");
        Bool_knob(f, &_aovIndirect, "aov_indirect", "indirect");
        Bool_knob(f, &_aovEmission, "aov_emission", "emission");
        Tooltip(f, "Light decomposition for comp relighting.<br>"
                   "beauty = direct + indirect + emission.");

        Divider(f, "LPE decomposition");
        Bool_knob(f, &_aovDiffuseDirect, "aov_diffuse_direct", "diffuse direct");
        Bool_knob(f, &_aovSpecularDirect, "aov_specular_direct", "specular direct");
        Newline(f);
        Bool_knob(f, &_aovDiffuseIndirect, "aov_diffuse_indirect", "diffuse indirect");
        Bool_knob(f, &_aovSpecularIndirect, "aov_specular_indirect", "specular indirect");
        Newline(f);
        Bool_knob(f, &_aovTransmission, "aov_transmission", "transmission");
        Tooltip(f, "Light Path Expression decomposition.<br>"
                   "diffuse direct = C&lt;RD&gt;L<br>"
                   "specular direct = C&lt;RS&gt;L<br>"
                   "diffuse indirect = C&lt;RD&gt;+L<br>"
                   "specular indirect = C&lt;RS&gt;+L<br>"
                   "transmission = C&lt;TS&gt;L");

        Divider(f, "Utility");
        Int_knob(f, &_aoSamples, "ao_samples", "AO samples"); SetRange(f, 0, 64);
        Float_knob(f, &_aoRadius, "ao_radius", "AO radius"); SetRange(f, 0.1f, 100.f);
        Tooltip(f, "Ambient occlusion. 0 samples = disabled.");
    }
    EndGroup(f);

    BeginClosedGroup(f, "spectral_engine", "Spectral rendering engine — how it works");
    {
        Text_knob(f,
            "<b>What makes this renderer different</b><br>"
            "Most production renderers work in RGB &mdash; every ray carries 3 colour channels.<br>"
            "SpectralRenderer traces one wavelength per ray (380-780nm), like real photons.<br>"
            "This means dispersion, thin-film interference, fluorescence and spectral absorption<br>"
            "happen physically &mdash; no faking, no post-process tricks.<br>"
            "<br>"
            "<b>The rendering equation (Kajiya 1986)</b><br>"
            "L<sub>o</sub>(p,&omega;<sub>o</sub>) = L<sub>e</sub>(p,&omega;<sub>o</sub>) "
            "+ &int; f(p,&omega;<sub>i</sub>,&omega;<sub>o</sub>) &middot; "
            "L<sub>i</sub>(p,&omega;<sub>i</sub>) &middot; cos&theta; d&omega;<sub>i</sub><br>"
            "In words: outgoing light = emission + all incoming light &times; surface reflectance.<br>"
            "We solve this with Monte Carlo &mdash; trace random rays, average the result.<br>"
            "More samples = less noise. The spectral version evaluates per-wavelength &lambda;.<br>"
            "<br>"
            "<b>How spectral sampling works</b><br>"
            "Each sample picks a random wavelength &lambda; &isin; [380, 780] nm.<br>"
            "Spectral radiance L(&lambda;) converts to CIE XYZ tristimulus:<br>"
            "X = &int; L(&lambda;) &middot; x&#772;(&lambda;) d&lambda; , "
            "Y = &int; L(&lambda;) &middot; y&#772;(&lambda;) d&lambda; , "
            "Z = &int; L(&lambda;) &middot; z&#772;(&lambda;) d&lambda;<br>"
            "XYZ &rarr; linear sRGB via 3&times;3 matrix. Monte Carlo estimates each integral<br>"
            "by averaging random &lambda; samples: X &asymp; (1/N) &sum; L(&lambda;<sub>i</sub>) &middot; x&#772;(&lambda;<sub>i</sub>).<br>"
            "RGB colour &rarr; spectral reflectance via Gaussian basis functions:<br>"
            "R(&lambda;) = r&middot;G(&lambda;,630,30) + g&middot;G(&lambda;,532,30) + b&middot;G(&lambda;,460,25)<br>"
            "where G(&lambda;,&mu;,&sigma;) = e<sup>-(&lambda;-&mu;)&sup2;/2&sigma;&sup2;</sup>.<br>"
            "<br>"
            "<b>Materials and shading</b><br>"
            "Disney BSDF: f = f<sub>diff</sub> + f<sub>spec</sub>. "
            "Specular: D&middot;G&middot;F / (4 cos&theta;<sub>i</sub> cos&theta;<sub>o</sub>).<br>"
            "D = GGX: &alpha;&sup2; / (&pi;(cos&sup2;&theta;<sub>h</sub>(&alpha;&sup2;-1)+1)&sup2;). "
            "G = Smith height-correlated.<br>"
            "Metals use measured spectral (n,k) optical constants &mdash; gold, copper, silver, etc.<br>"
            "have physically correct colour that shifts with viewing angle.<br>"
            "Fresnel conductor: F = (R<sub>s</sub>+R<sub>p</sub>)/2 with complex refractive index.<br>"
            "Multiscatter GGX (Kulla-Conty): compensates energy lost to multiple microfacet bounces.<br>"
            "<br>"
            "<b>Glass and dispersion</b><br>"
            "Transparent materials refract using Snell's law: n<sub>1</sub>sin&theta;<sub>1</sub> = n<sub>2</sub>sin&theta;<sub>2</sub>.<br>"
            "The Abbe number controls how much each wavelength &lambda; bends differently.<br>"
            "Crown glass (V<sub>d</sub>=58) gives subtle dispersion. Flint glass (30) gives strong rainbows.<br>"
            "Diamond (V<sub>d</sub>=55, IOR 2.42) produces dramatic fire effects.<br>"
            "<br>"
            "<b>Beer-Lambert volumetric absorption</b><br>"
            "Light traveling through glass/liquid is absorbed: T(&lambda;) = e<sup>-&sigma;(&lambda;)&middot;d</sup><br>"
            "where &sigma;(&lambda;) = -ln(color<sub>&lambda;</sub>) &times; density, and d = distance inside the medium.<br>"
            "Red glass absorbs blue/green wavelengths. Thicker glass = deeper colour.<br>"
            "Each wavelength is absorbed independently &mdash; physically correct spectral absorption.<br>"
            "<br>"
            "<b>Caustics</b><br>"
            "Light concentrations from refraction (glass focusing) or reflection (curved mirrors).<br>"
            "Currently disabled for stability &mdash; will be re-implemented with progressive<br>"
            "photon mapping in a future update for production-quality rainbow caustics.<br>"
            "<br>"
            "<b>Noise reduction</b><br>"
            "Multiple Importance Sampling (MIS) combines light and surface sampling strategies<br>"
            "to reduce noise — especially effective for glossy surfaces under small bright lights.<br>"
            "Adaptive sampling stops sending rays to pixels that are already clean.<br>"
            "Blue noise (R2 sequence) distributes samples more evenly than pure random.<br>"
            "OptiX AI denoiser can clean up remaining noise as a post-process.<br>"
            "<br>"
            "<b>Depth of field</b><br>"
            "Thin lens model: f-stop controls aperture size, focus distance sets the sharp plane.<br>"
            "Each ray origin is jittered on a circular lens disk — objects away from the focus<br>"
            "plane blur naturally. Lower f-stop = shallower DOF. f/0 = pinhole (everything sharp).<br>"
            "<br>"
            "<b>Environment and lighting</b><br>"
            "HDRI environment maps provide image-based lighting from all directions.<br>"
            "Each shading point samples the hemisphere around its surface normal.<br>"
            "CIE illuminants (D65, D50, A, etc.) provide physically standardised light spectra.<br>"
            "Shadow rays pass through glass with Fresnel-weighted transmittance.<br>"
            "<br>"
            "<b>Metal presets — spectral Fresnel conductor</b><br>"
            "Gold: warm yellow, shifts to white at grazing. Absorbs blue wavelengths.<br>"
            "Copper: reddish face-on, whitens at edges. Sharp spectral transition at 580nm.<br>"
            "Silver: near-neutral, very high reflectance across all wavelengths.<br>"
            "Aluminium: slightly warm neutral, subtle blue-shift at grazing angles.<br>"
            "Iron/Titanium: dark metals with strong wavelength-dependent absorption."
        );
    }
    EndGroup(f);

    Divider(f);
    Text_knob(f,
        "<font color='#666' size='-1'>"
        "SpectralRenderer v1.0 \xc2\xb7 NDK + Hydra \xc2\xb7 Nuke 17 \xc2\xb7 Embree 4 \xc2\xb7 OptiX 8.1 \xc2\xb7 OpenSubdiv 3.6<br>"
        "Created by Marten Blumen"
        "</font>"
    );
}

int SpectralRenderIop::knob_changed(Knob* k)
{
    _frameReady.store(false);
    _progressiveSppDone = 0;

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
        _frameReady.store(false);
        _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
        _vdbLoadedPath.clear();
        return 1;
    }
    if (k->is("vdb_show_points") || k->is("vdb_point_density") || k->is("vdb_point_size") || k->is("vdb_show_bbox") || k->is("vdb_fast_scrub")) {
        _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
        if (k->is("vdb_fast_scrub")) _vdbLoadedPath.clear();
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
        _frameReady.store(false);
#else
        error("OpenVDB not compiled.");
#endif
        return 1;
    }

    // Sky preset
    if (k->is("sky_preset")) {
        switch (_skyPreset) {
            case 0: _skyMix = 0; break;  // Off
            case 2: _sunElevation=60; _sunAzimuth=180; _sunIntensity=5; _skyIntensity=1; _turbidity=2.5; _skyMix=1; break; // Day
            case 3: _sunElevation=8; _sunAzimuth=240; _sunIntensity=4; _skyIntensity=0.6; _turbidity=4; _skyMix=1; break; // Golden
            case 4: _sunElevation=50; _sunAzimuth=180; _sunIntensity=0.5; _skyIntensity=1.5; _turbidity=8; _skyMix=1; break; // Overcast
            case 5: _sunElevation=2; _sunAzimuth=270; _sunIntensity=0.3; _skyIntensity=0.4; _turbidity=3; _skyMix=1; break; // Blue Hour
            case 6: _sunElevation=30; _sunAzimuth=180; _sunIntensity=0.2; _skyIntensity=0.05; _turbidity=2; _skyMix=0.3; break; // Night
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
        _frame = currentFrame;
        _frameReady.store(false);
        _progressiveSppDone = 0;
        _vdbLastLoadedFrame = -999; // force VDB reload for new frame
        _volume.reset();
    }
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

    if (forReal) {
        _LoadStage();
        _BuildCameraFromInput();
        _frameReady.store(false);
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
    _EnsureFrameRendered();

    const int W = static_cast<int>(_fbWidth);       // proxy render resolution
    const int H = static_cast<int>(_fbHeight);
    const int fullW = static_cast<int>(_fbFullWidth ? _fbFullWidth : _fbWidth);
    const int fullH = static_cast<int>(_fbFullHeight ? _fbFullHeight : _fbHeight);

    int fullBufY = fullH - 1 - y;
    // Map output Y to proxy buffer Y
    int bufY = (H == fullH) ? fullBufY : (fullBufY * H / fullH);

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
    _camera = SpectralCamera();

    // ------------------------------------------------------------------
    // Path 1: GeomOp input connected — use Nuke's USD scene graph
    // ------------------------------------------------------------------
    Op* in0 = (maximum_inputs() > 0 && inputs() > 0) ? input(0) : nullptr;
    GeometryProviderI* geoProvider = in0 ? dynamic_cast<GeometryProviderI*>(in0) : nullptr;

    if (geoProvider) {
        fprintf(stderr, "SpectralRender: reading from node graph input\n");

        try {
            // Validate the upstream op
            in0->validate(true);

            // Build a usg::Stage from the upstream GeomOp
            usg::StageRef usgStage = usg::Stage::CreateInMemory();
            if (!usgStage) {
                fprintf(stderr, "SpectralRender: failed to create in-memory stage\n");
                return;
            }

            // Use the current frame time
            fdk::TimeValue sampleTime(static_cast<double>(_frame));
            OpGraphLocation location(in0);
            GeometryProviderI::BuildStage(usgStage, location, sampleTime);

            if (!usgStage || !usgStage->isValid()) {
                fprintf(stderr, "SpectralRender: BuildStage produced invalid stage\n");
                return;
            }

            // Extract the PXR UsdStageRefPtr from the usg::Stage
            int usdVer = usg::usdAPIVersion();
            usg::Stage::Handle* handle = usgStage->getUsdStageRefPtr(usdVer);
            if (!handle) {
                fprintf(stderr, "SpectralRender: getUsdStageRefPtr failed (version mismatch?)\n");
                return;
            }

            // Cast to PXR UsdStageRefPtr
            UsdStageRefPtr* pxrStagePtr = reinterpret_cast<UsdStageRefPtr*>(handle);
            if (!pxrStagePtr || !(*pxrStagePtr)) {
                fprintf(stderr, "SpectralRender: PXR stage pointer is null\n");
                return;
            }

            fprintf(stderr, "SpectralRender: got PXR stage from node graph\n");
            _LoadFromPxrStage(*pxrStagePtr);
            return;

        } catch (const std::exception& e) {
            fprintf(stderr, "SpectralRender: node graph input error: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "SpectralRender: node graph input unknown error\n");
        }
    }

    // ------------------------------------------------------------------
    // Path 2: File knob fallback
    // ------------------------------------------------------------------
    if (!_usdFilePath || _usdFilePath[0] == '\0') {
        fprintf(stderr, "SpectralRender: no input connected and no USD file set\n");
        return;
    }

    fprintf(stderr, "SpectralRender: opening file %s\n", _usdFilePath);

    UsdStageRefPtr stage;
    try {
        stage = UsdStage::Open(std::string(_usdFilePath));
    } catch (const std::exception& e) {
        fprintf(stderr, "SpectralRender: exception opening stage: %s\n", e.what());
        return;
    } catch (...) {
        fprintf(stderr, "SpectralRender: unknown exception opening stage\n");
        return;
    }
    if (!stage) {
        fprintf(stderr, "SpectralRender: UsdStage::Open returned null\n");
        return;
    }

    fprintf(stderr, "SpectralRender: stage opened OK from file\n");
    _LoadFromPxrStage(stage);
}

// ---------------------------------------------------------------------------
// _LoadFromPxrStage — shared mesh/camera loading from a PXR UsdStageRefPtr
// ---------------------------------------------------------------------------
void SpectralRenderIop::_LoadFromPxrStage(const UsdStageRefPtr& stage)
{
    const UsdTimeCode timeCode(_frame);
    const UsdTimeCode timeOpen(_frame + _shutterOpen);
    const UsdTimeCode timeClose(_frame + _shutterClose);
    const bool motionBlurEnabled = (_shutterOpen != _shutterClose);
    UsdGeomXformCache xfCache(timeCode);

    int meshCount = 0;
    int totalTris = 0;
    UsdPrim cameraPrim;

    // If user specified a camera path, try that first
    if (_cameraPath && _cameraPath[0] != '\0') {
        UsdPrim p = stage->GetPrimAtPath(SdfPath(std::string(_cameraPath)));
        if (p.IsValid() && p.IsA<UsdGeomCamera>()) {
            cameraPrim = p;
            fprintf(stderr, "SpectralRender: using specified camera: %s\n", _cameraPath);
        } else {
            fprintf(stderr, "SpectralRender: camera not found at %s\n", _cameraPath);
        }
    }

    for (const UsdPrim& prim : stage->Traverse()) {
        // Auto-find first camera
        if (!cameraPrim.IsValid() && prim.IsA<UsdGeomCamera>()) {
            cameraPrim = prim;
            fprintf(stderr, "SpectralRender: auto-found camera: %s\n",
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
                // Try to load HDRI texture
                UsdLuxDomeLight dome(prim);
                SdfAssetPath texPath;
                if (dome.GetTextureFileAttr().Get(&texPath, timeCode)) {
                    std::string filePath = texPath.GetResolvedPath();
                    if (filePath.empty()) filePath = texPath.GetAssetPath();
                    if (!filePath.empty()) {
                        int texId = _scene->LoadTexture(filePath);
                        if (texId >= 0) {
                            const auto* tex = _scene->GetTexture(texId);
                            if (tex && tex->IsValid()) {
                                light.envTexId = texId;
                                light.envWidth = tex->GetWidth();
                                light.envHeight = tex->GetHeight();
                                light.envPixels = tex->_pixels.data();
                                fprintf(stderr, "SpectralRender: HDRI env map '%s' (%dx%d)\n",
                                        filePath.c_str(), light.envWidth, light.envHeight);
                            }
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
            fprintf(stderr, "SpectralRender: light '%s' — type=%d color=(%.2f,%.2f,%.2f) "
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

            fprintf(stderr, "SpectralRender: PointInstancer '%s' — %zu instances, %zu prototypes\n",
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
                _scene->SetMeshData(meshData.id, std::move(meshData));
            }
            continue;
        }

        // Process meshes
        if (!prim.IsA<UsdGeomMesh>()) continue;

        // Skip SpectralVDBRead placeholder bbox (rendered as volume, not geometry)
        std::string primName = prim.GetPath().GetString();
        if (primName.find("SpectralVDBRead") != std::string::npos) {
            fprintf(stderr, "SpectralRender: skipping VDB placeholder mesh %s\n", primName.c_str());
            continue;
        }

        UsdGeomMesh mesh(prim);
        if (!mesh) continue;

        VtVec3fArray points;
        mesh.GetPointsAttr().Get(&points, timeOpen);
        if (points.empty()) continue;
        VtVec3fArray pointsRef;  // undisplaced positions for pRef AOV

        // Read points at shutter close for motion blur
        VtVec3fArray pointsClose;
        bool meshHasMotion = false;
        if (motionBlurEnabled) {
            mesh.GetPointsAttr().Get(&pointsClose, timeClose);

            // Check if local positions differ (vertex deformation)
            if (pointsClose.size() == points.size()) {
                for (size_t pi = 0; pi < points.size(); ++pi) {
                    if ((points[pi] - pointsClose[pi]).GetLengthSq() > 1e-10f) {
                        meshHasMotion = true;
                        break;
                    }
                }
            }

            // Check if the world transform differs (rigid body motion)
            if (!meshHasMotion) {
                UsdGeomXformCache xfCacheOpen(timeOpen);
                UsdGeomXformCache xfCacheClose(timeClose);
                GfMatrix4d xfOpen  = xfCacheOpen.GetLocalToWorldTransform(prim);
                GfMatrix4d xfClose = xfCacheClose.GetLocalToWorldTransform(prim);
                if (xfOpen != xfClose) {
                    meshHasMotion = true;
                    // Use the same points but they'll get different transforms
                    pointsClose = points;
                }
            }

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
                    fprintf(stderr, "SpectralRender: shader id='%s' prim='%s'\n",
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
                            fprintf(stderr, "  input '%s' -> connected to '%s' (id='%s')\n",
                                    inp.GetBaseName().GetText(),
                                    connSrc.GetPrim().GetPath().GetText(),
                                    connId.GetText());
                        } else if (val.IsHolding<SdfAssetPath>()) {
                            fprintf(stderr, "  input '%s' = asset:'%s'\n",
                                    inp.GetBaseName().GetText(),
                                    val.UncheckedGet<SdfAssetPath>().GetAssetPath().c_str());
                        }
                    }

                    // Read UsdPreviewSurface inputs
                    auto readFloat = [&](const char* inputName, float& val) {
                        UsdShadeInput inp = surfaceShader.GetInput(TfToken(inputName));
                        if (inp) {
                            VtValue v;
                            inp.Get(&v, timeCode);
                            if (v.IsHolding<float>()) val = v.UncheckedGet<float>();
                        }
                    };
                    auto readColor = [&](const char* inputName, GfVec3f& val) {
                        UsdShadeInput inp = surfaceShader.GetInput(TfToken(inputName));
                        if (inp) {
                            VtValue v;
                            inp.Get(&v, timeCode);
                            if (v.IsHolding<GfVec3f>()) val = v.UncheckedGet<GfVec3f>();
                        }
                    };

                    readColor("diffuseColor", mat.baseColor);
                    readFloat("metallic", mat.metallic);
                    readFloat("roughness", mat.roughness);
                    readFloat("ior", mat.ior);
                    readFloat("opacity", mat.opacity);
                    readColor("emissiveColor", mat.emissiveColor);
                    readFloat("clearcoat", mat.clearcoat);
                    readFloat("clearcoatRoughness", mat.clearcoatRoughness);

                    // Determine texture input names based on shader type
                    const char* diffuseTexInput = "diffuseColor";
                    const char* roughTexInput   = "roughness";
                    const char* metalTexInput   = "metallic";

                    if (surfShaderId == TfToken("NukeDefaultSurface")) {
                        // Nuke's default surface uses different input names
                        diffuseTexInput = "tex_color";
                        // NukeDefaultSurface has tex_color/tex_opacity but
                        // no separate roughness/metallic texture inputs
                        roughTexInput = nullptr;
                        metalTexInput = nullptr;
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
                                            tex._channels = 3;
                                            tex._pixels.resize(size_t(texW) * texH * 3);
                                            tex._path = assetPath;

                                            texIop->request(0, 0, texW, texH, Mask_RGB, 1);
                                            for (int y = 0; y < texH; ++y) {
                                                Row row(0, texW);
                                                texIop->get(y, 0, texW, Mask_RGB, row);
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

                                            int texId = _scene->AddTexture(std::move(tex));
                                            fprintf(stderr, "SpectralRender: Iop texture '%s' -> %s (%dx%d, id=%d)\n",
                                                    inputName, texIop->node_name(), texW, texH, texId);
                                            return texId;
                                        }
                                    } catch (...) {
                                        fprintf(stderr, "SpectralRender: failed to render Iop texture '%s'\n",
                                                inputName);
                                    }
                                }
                                return -1;
                            }

                            // Real file on disk
                            int texId = _scene->LoadTexture(filePath);
                            if (texId >= 0) {
                                fprintf(stderr, "SpectralRender: texture '%s' -> '%s' (id=%d)\n",
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
                                            tex._channels = 3;
                                            tex._pixels.resize(size_t(texW) * texH * 3);
                                            tex._path = assetPath;

                                            texIop->request(0, 0, texW, texH, Mask_RGB, 1);
                                            for (int y = 0; y < texH; ++y) {
                                                Row row(0, texW);
                                                texIop->get(y, 0, texW, Mask_RGB, row);
                                                const float* rp = row[Chan_Red] ? row[Chan_Red] : nullptr;
                                                const float* gp = row[Chan_Green] ? row[Chan_Green] : nullptr;
                                                const float* bp = row[Chan_Blue] ? row[Chan_Blue] : nullptr;
                                                // Store top-down (row 0 = top of image)
                                                int storeY = texH - 1 - y;
                                                for (int x = 0; x < texW; ++x) {
                                                    size_t idx = (size_t(storeY) * texW + x) * 3;
                                                    tex._pixels[idx + 0] = rp ? rp[x] : 0.f;
                                                    tex._pixels[idx + 1] = gp ? gp[x] : 0.f;
                                                    tex._pixels[idx + 2] = bp ? bp[x] : 0.f;
                                                }
                                            }

                                            // Add to texture table
                                            int texId = _scene->AddTexture(std::move(tex));
                                            fprintf(stderr, "SpectralRender: Iop texture '%s' -> %s (%dx%d, id=%d)\n",
                                                    inputName, texIop->node_name(), texW, texH, texId);
                                            return texId;
                                        }
                                    } catch (...) {
                                        fprintf(stderr, "SpectralRender: failed to render Iop texture '%s'\n",
                                                inputName);
                                    }
                                }
                            }
                        }

                        fprintf(stderr, "SpectralRender: unsupported texture shader '%s' on '%s'\n",
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
                                        fprintf(stderr, "SpectralRender: displacement map '%s' (id=%d)\n",
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
                                                fprintf(stderr, "SpectralRender: displacement from Iop '%s' (%dx%d, id=%d)\n",
                                                        dispIop->node_name(), texW, texH, mat.displacementTexId);
                                            }
                                        } catch (...) {
                                            fprintf(stderr, "SpectralRender: failed to read displacement Iop\n");
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
                                                tex._channels = 3;
                                                tex._pixels.resize(size_t(texW) * texH * 3);
                                                tex._path = "tex_iop";
                                                texIop->request(0, 0, texW, texH, Mask_RGB, 1);
                                                for (int y = 0; y < texH; ++y) {
                                                    Row row(0, texW);
                                                    texIop->get(y, 0, texW, Mask_RGB, row);
                                                    const float* rp = row[Chan_Red];
                                                    const float* gp = row[Chan_Green];
                                                    const float* bp = row[Chan_Blue];
                                                    int storeY = texH - 1 - y;
                                                    for (int x = 0; x < texW; ++x) {
                                                        size_t idx = (size_t(storeY) * texW + x) * 3;
                                                        tex._pixels[idx+0] = rp ? rp[x] : 0.f;
                                                        tex._pixels[idx+1] = gp ? gp[x] : 0.f;
                                                        tex._pixels[idx+2] = bp ? bp[x] : 0.f;
                                                    }
                                                }
                                                mat.baseColorTexId = _scene->AddTexture(std::move(tex));
                                                fprintf(stderr, "SpectralRender: base color from tex pipe '%s' (%dx%d, id=%d)\n",
                                                        texIop->node_name(), texW, texH, mat.baseColorTexId);
                                            }
                                        } catch (...) {
                                            fprintf(stderr, "SpectralRender: failed to read texture Iop\n");
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
                                                fprintf(stderr, "SpectralRender: bump map from pipe '%s' (%dx%d, id=%d, strength=%.2f)\n",
                                                        bumpIop->node_name(), bW, bH, mat.bumpMapTexId, mat.bumpStrength);
                                            }
                                        } catch (...) {
                                            fprintf(stderr, "SpectralRender: failed to read bump Iop\n");
                                        }
                                    }
                                }

                                if (mat.abbeNumber > 0.f || mat.thinFilmThickness > 0.f
                                    || mat.displacementScale > 0.f) {
                                    fprintf(stderr, "SpectralRender: spectral props from '%s'"
                                            " — Abbe=%.1f thinFilm=%.0fnm disp=%.2f\n",
                                            entry.first.c_str(),
                                            mat.abbeNumber, mat.thinFilmThickness,
                                            mat.displacementScale);
                                }
                                break;
                            }
                        }
                    }

                    matId = _scene->AddMaterial(mat);

                    fprintf(stderr, "SpectralRender: material '%s' — color=(%.2f,%.2f,%.2f) metal=%.2f rough=%.2f opacity=%.2f\n",
                            mat.name.c_str(),
                            mat.baseColor[0], mat.baseColor[1], mat.baseColor[2],
                            mat.metallic, mat.roughness, mat.opacity);
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

            // Auto-subdivide when displacement is present but no scheme set
            const SpectralMaterial& meshMat = _scene->GetMaterial(matId);
            if (scheme == SpectralSubdiv::Scheme::None
                && meshMat.displacementScale > 0.f
                && meshMat.displacementTexId >= 0) {
                scheme = SpectralSubdiv::Scheme::CatmullClark;
                fprintf(stderr, "SpectralRender: auto-subdividing %s for displacement\n",
                        prim.GetPath().GetText());
            }

            if (scheme != SpectralSubdiv::Scheme::None) {
                SpectralSubdiv::Input subIn;
                subIn.points            = points;
                subIn.faceVertexCounts  = faceVertexCounts;
                subIn.faceVertexIndices = faceVertexIndices;
                subIn.scheme            = scheme;
                subIn.level             = 2;

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
                            fprintf(stderr, "SpectralRender: %zu crease edges (sharpness %.1f-%.1f)\n",
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
                        fprintf(stderr, "SpectralRender: %zu corner vertices\n", cornerIndices.size());
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

                    fprintf(stderr, "SpectralRender: mesh %s subdivided (%s level 2)\n",
                            prim.GetPath().GetText(), schemeStr.c_str());

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
                        fprintf(stderr, "SpectralRender: refined %zu UVs for %s\n",
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

                                fprintf(stderr, "SpectralRender: displaced %d vertices (scale=%.2f, tex=%dx%d) for %s\n",
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

        // World transform (at shutter open if motion blur, else at frame)
        GfMatrix4d worldXf;
        if (motionBlurEnabled) {
            UsdGeomXformCache xfCacheOpen(timeOpen);
            worldXf = xfCacheOpen.GetLocalToWorldTransform(prim);
        } else {
            worldXf = xfCache.GetLocalToWorldTransform(prim);
        }
        GfMatrix4d normalXf = worldXf.GetInverse().GetTranspose();

        // Close-time transform for motion blur
        GfMatrix4d worldXfClose = worldXf;
        if (meshHasMotion) {
            UsdGeomXformCache xfCacheClose(timeClose);
            worldXfClose = xfCacheClose.GetLocalToWorldTransform(prim);
        }

        const int numPoints = static_cast<int>(points.size());

        fprintf(stderr, "SpectralRender: mesh %s — %d points, %d faces, %d normals (%s)\n",
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
            _scene->SetMeshData(data.id, std::move(data));
            meshCount++;
        }
    }

    fprintf(stderr, "SpectralRender: loaded %d meshes, %d triangles total\n",
            meshCount, totalTris);

    // ------------------------------------------------------------------
    // Detect USD Volume prims (OpenVDBAsset) in the stage
    // ------------------------------------------------------------------
    // Detect volume prims in the USD stage:
    //   1. Standard UsdVol::Volume + OpenVDBAsset field prims  
    //   2. (SpectralVDBRead volumes found via input chain in _LoadVDB)
    // ------------------------------------------------------------------
#ifdef SPECTRAL_HAS_VDB
    if (!_volume || !_volume->IsValid()) {
        for (const auto& prim : stage->Traverse()) {
            if (!prim.IsA<UsdVolVolume>()) continue;

            UsdVolVolume volumePrim(prim);
            if (!volumePrim) continue;

            std::string densityPath, tempPath;
            for (const auto& child : prim.GetChildren()) {
                if (!child.IsA<UsdVolOpenVDBAsset>()) continue;
                UsdVolOpenVDBAsset asset(child);
                if (!asset) continue;

                SdfAssetPath filePath;
                asset.GetFilePathAttr().Get(&filePath);
                TfToken fieldName;
                asset.GetFieldNameAttr().Get(&fieldName);

                std::string resolvedFile = filePath.GetResolvedPath();
                if (resolvedFile.empty()) resolvedFile = filePath.GetAssetPath();

                if (fieldName == "density" || fieldName == "smoke" || fieldName == "soot") {
                    densityPath = resolvedFile;
                } else if (fieldName == "temperature" || fieldName == "temp" || fieldName == "heat") {
                    tempPath = resolvedFile;
                }
            }

            if (!densityPath.empty()) {
                const char* tempArg = tempPath.empty() ? nullptr : "temperature";
                _volume = pxr::SpectralVDBLoader::Load(densityPath.c_str(), "density", tempArg);
                if (_volume && _volume->IsValid()) {
                    _applyVolumeShading(_volume);
                    fprintf(stderr, "SpectralRender: loaded USD Volume prim %s (%dx%dx%d)\n",
                            prim.GetPath().GetText(), _volume->resX, _volume->resY, _volume->resZ);
                    break;
                }
            }
        }
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

    fprintf(stderr, "SpectralRender: format %dx%d pixel_aspect=%.4f\n",
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
            fprintf(stderr, "SpectralRender: camera at (%.2f, %.2f, %.2f)\n",
                    camPos[0], camPos[1], camPos[2]);
        } catch (...) {
            fprintf(stderr, "SpectralRender: camera extraction failed\n");
        }
    }

    if (!foundCamera) {
        fprintf(stderr, "SpectralRender: using default 50mm camera at origin\n");
        _camera.viewToWorld = GfMatrix4d(1.0);
        const double fov   = 50.0 * M_PI / 180.0;
        const double near_ = 0.1, far_ = 10000.0;
        const double f     = 1.0 / std::tan(fov * 0.5);
        GfMatrix4d proj(0.0);
        proj[0][0] = f / aspect;
        proj[1][1] = f;
        proj[2][2] = (far_ + near_) / (near_ - far_);
        proj[2][3] = -1.0;
        proj[3][2] = (2.0 * far_ * near_) / (near_ - far_);
        _camera.projInverse = proj.GetInverse();
    }
}

// ---------------------------------------------------------------------------
// _BuildCameraFromInput — override camera from input 1 (CameraOp) if connected
// ---------------------------------------------------------------------------
void SpectralRenderIop::_BuildCameraFromInput()
{
    _cameraFromInput = false;

    Op* camOp = (inputs() > 1) ? input(1) : nullptr;
    if (!camOp) return;

    CameraOp* cam = dynamic_cast<CameraOp*>(camOp);
    if (!cam) return;

    try {
        cam->validate(true);
    } catch (...) {
        fprintf(stderr, "SpectralRender: camera input validation failed\n");
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

    GfVec3d camPos = _camera.viewToWorld.ExtractTranslation();
    fprintf(stderr, "SpectralRender: camera from input at (%.2f, %.2f, %.2f)\n",
            camPos[0], camPos[1], camPos[2]);
    fprintf(stderr, "SpectralRender: DDImage proj[0][0]=%.4f [1][1]=%.4f → corrected [1][1]=%.4f (aspect=%.4f)\n",
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
    hash.append(outputContext().frame());
    hash.append(_vdbFrameOffset);
    if (_vdbFile) hash.append(_vdbFile);
}

// ---------------------------------------------------------------------------
// build_handles / draw_handle — wireframe bbox + point cloud in 3D viewport
// ---------------------------------------------------------------------------
void SpectralRenderIop::build_handles(ViewerContext* ctx)
{
    if (ctx->transform_mode() == VIEWER_2D) return;
    // _volume loaded by _validate — draw if bbox is valid (even metadata-only)
    if (_volume && _volume->HasBbox()) add_draw_handle(ctx);
}

void SpectralRenderIop::draw_handle(ViewerContext* ctx)
{
    if (!_volume || !_volume->HasBbox()) return;
    glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT | GL_ENABLE_BIT | GL_POINT_BIT);
    glDisable(GL_LIGHTING);

    float co[8][3];
    for (int i = 0; i < 8; ++i) {
        co[i][0] = (i & 1) ? _volume->bboxMax[0] : _volume->bboxMin[0];
        co[i][1] = (i & 2) ? _volume->bboxMax[1] : _volume->bboxMin[1];
        co[i][2] = (i & 4) ? _volume->bboxMax[2] : _volume->bboxMin[2];
    }
    static const int edges[12][2] = {
        {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}
    };

    if (_vdbShowBbox) {
        glLineWidth(1.5f); glColor3f(0, 1, 0);
        glBegin(GL_LINES);
        for (int e = 0; e < 12; ++e) { glVertex3fv(co[edges[e][0]]); glVertex3fv(co[edges[e][1]]); }
        glEnd();
    }

    if (_vdbShowPoints && !_vdbIsMetadataOnly) {
        if (_vdbPreviewDirty || _vdbPreviewPoints.empty()) {
            _vdbPreviewPoints.clear(); _vdbMaxDensity = 1e-6f;
            pxr::GfVec3f bMin = _volume->bboxMin, bSz = _volume->bboxMax - _volume->bboxMin;
            int step = std::max(1, int(1.f / float(std::max(0.05, _vdbPointDensity))));
            for (int iz = 0; iz < _volume->resZ; iz += step)
                for (int iy = 0; iy < _volume->resY; iy += step)
                    for (int ix = 0; ix < _volume->resX; ix += step) {
                        float u = float(ix)/_volume->resX, v = float(iy)/_volume->resY, w = float(iz)/_volume->resZ;
                        float d = _volume->SampleDensity(u, v, w);
                        if (d < 0.01f) continue;
                        if (d > _vdbMaxDensity) _vdbMaxDensity = d;
                        VDBPreviewPoint pt; pt.x = bMin[0]+u*bSz[0]; pt.y = bMin[1]+v*bSz[1]; pt.z = bMin[2]+w*bSz[2]; pt.density = d;
                        _vdbPreviewPoints.push_back(pt);
                    }
            _vdbPreviewDirty = false;
        }
        if (!_vdbPreviewPoints.empty()) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glPointSize(float(_vdbPointSize)); glEnable(GL_POINT_SMOOTH);
            float inv = 1.f / _vdbMaxDensity;
            glBegin(GL_POINTS);
            for (const auto& pt : _vdbPreviewPoints) {
                float t = std::min(pt.density * inv, 1.f);
                glColor4f(std::min(t*3,1.f), std::max(0.f,std::min((t-.33f)*3,1.f)),
                          std::max(0.f,std::min((t-.66f)*3,1.f)), .15f+.85f*t);
                glVertex3f(pt.x, pt.y, pt.z);
            }
            glEnd(); glDisable(GL_BLEND);
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
        _applyVolumeShading(_volume);
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
                    // Full load at viewport res (64³)
                    _volume = pxr::SpectralVDBLoader::Load(resolvedPath.c_str(),
                        _VdbGridName(_vdbDensityGridIdx, _vdbDensityOverride),
                        _VdbGridName(_vdbTempGridIdx, _vdbTempOverride),
                        64);
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

    // Path 2: Vol input (input 3) — SpectralVDBRead
    if (inputs() > 3 && input(3)) {
        input(3)->validate(true);
        SpectralVDBRead* vdbRead = nullptr;
        if (strcmp(input(3)->Class(), "SpectralVDBRead") == 0)
            vdbRead = static_cast<SpectralVDBRead*>(static_cast<Op*>(input(3)));
        if (vdbRead) {
            int renderFrame = int(outputContext().frame());
            auto vol = vdbRead->GetVolumeAtFrame(renderFrame);
            if (vol && vol->IsValid()) {
                _applyVolumeShading(vol);
                _volume = vol;
                _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// _LoadVDBForRender — reload at full 128³ if currently at preview res
// ---------------------------------------------------------------------------
void SpectralRenderIop::_LoadVDBForRender()
{
#ifdef SPECTRAL_HAS_VDB
    if ((_vdbIsPreviewRes || _vdbIsMetadataOnly) && _vdbFile && strlen(_vdbFile) > 0) {
        int frame = int(outputContext().frame()) + _vdbFrameOffset;
        std::string resolvedPath = _vdbAutoSequence
            ? _resolveFramePath(frame)
            : std::string(_vdbFile);
        if (!resolvedPath.empty()) {
            // Check if a full-res version is in cache
            std::string renderKey = resolvedPath + ":128";
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
                128);
            _vdbIsPreviewRes = false;
            _vdbIsMetadataOnly = false;
            if (_volume) {
                _vdbLoadedPath = resolvedPath;
                _applyVolumeShading(_volume);
                _VDBCachePut(renderKey, _volume, false, false);
            }
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// _applyVolumeShading — copy shading params from knobs to volume
// ---------------------------------------------------------------------------
void SpectralRenderIop::_applyVolumeShading(std::shared_ptr<pxr::SpectralVolume>& vol)
{
    // Check VolMat input (input 4) for SpectralVolumeMaterial
    // Use Class() string match — dynamic_cast fails across MSVC dllexport TUs
    SpectralVolumeMaterial* mat = nullptr;
    if (inputs() > 4 && input(4)) {
        if (strcmp(input(4)->Class(), "SpectralVolumeMaterial") == 0) {
            mat = static_cast<SpectralVolumeMaterial*>(static_cast<Op*>(input(4)));
        }
    }

    if (mat) {
        // Use connected SpectralVolumeMaterial parameters
        vol->extinction = float(mat->extinction);
        vol->scattering = float(mat->scattering);
        vol->densityMult = float(mat->densityMult);
        vol->emissionIntensity = float(mat->emissionIntensity);
        vol->tempMin = float(mat->tempMin);
        vol->tempMax = float(mat->tempMax);
        vol->stepSize = float(mat->stepSize);
        vol->gForward = float(mat->gForward);
        vol->gBackward = float(mat->gBackward);
        vol->lobeMix = float(mat->lobeMix);
        vol->powderStrength = float(mat->powder);
        vol->jitter = mat->jitter;
        vol->scatterColor = pxr::GfVec3f(mat->scatterColor[0], mat->scatterColor[1], mat->scatterColor[2]);
        vol->useBlackbody = mat->useBlackbody;
        vol->chromaticExtinction = mat->chromaticExtinction;
        vol->sigmaR = float(mat->sigmaR);
        vol->sigmaG = float(mat->sigmaG);
        vol->sigmaB = float(mat->sigmaB);
    } else {
        // Fall back to SpectralRender's own Volumes tab knobs
        vol->extinction = float(_vdbExtinction);
        vol->scattering = float(_vdbScattering);
        vol->densityMult = float(_vdbDensityMult);
        vol->anisotropy = float(_vdbAnisotropy);
        vol->emissionIntensity = float(_vdbEmissionIntensity);
        vol->tempMin = float(_vdbTempMin);
        vol->tempMax = float(_vdbTempMax);
        vol->stepSize = float(_vdbStepSize);
        vol->gForward = float(_vdbGForward);
        vol->gBackward = float(_vdbGBackward);
        vol->lobeMix = float(_vdbLobeMix);
        vol->powderStrength = float(_vdbPowder);
        vol->jitter = _vdbJitter;
        vol->scatterColor = pxr::GfVec3f(_vdbScatterColor[0], _vdbScatterColor[1], _vdbScatterColor[2]);
        vol->useBlackbody = false;
        vol->chromaticExtinction = false;
    }
    // Always apply from SpectralRender (these aren't on VolumeMaterial yet)
    vol->shadowSteps = _vdbShadowSteps;
    vol->shadowDensity = float(_vdbShadowDensity);
    vol->quality = float(_vdbQuality);
    vol->adaptiveStep = _vdbAdaptiveStep;
    vol->msApprox = _vdbMsApprox;
    vol->msTint = pxr::GfVec3f(_vdbMsTint[0], _vdbMsTint[1], _vdbMsTint[2]);
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
    double t = std::max(0.0, std::min(elev / 90.0, 1.0));
    double turbShift = (turbidity - 2.0) / 8.0 * 0.15;
    r = 1.0;
    g = std::max(0.15, std::min(0.4 + 0.55 * t - turbShift, 1.0));
    b = std::max(0.02, std::min(0.1 + 0.8 * t * t - turbShift * 2, 0.95));
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

    // Sun/sky (Preetham analytical model)
    if (_skyPreset > 0 && _skyMix > 0.001) {
        double m = _skyMix;
        double sunR, sunG, sunB, skyR, skyG, skyB;
        sunColorFromElevation(_sunElevation, _turbidity, sunR, sunG, sunB);
        skyColorFromElevation(_sunElevation, _turbidity, skyR, skyG, skyB);
        GfVec3f sunDir = dirFromElevAzim(_sunElevation, _sunAzimuth);

        // Direct sun — use sphere light for soft shadows
        double si = _sunIntensity * std::max(0.1, std::min(_sunElevation / 15.0, 1.0)) * m;
        if (si > 0.001) {
            SpectralLight sun;
            if (_shadowSoftness > 0.01) {
                // Sphere light at distance for soft shadows
                sun.type = SpectralLight::Type::Sphere;
                float dist = 500.f;
                sun.position = GfVec3f(sunDir[0]*-dist, sunDir[1]*-dist, sunDir[2]*-dist);
                sun.radius = float(_shadowSoftness * 50.f);
            } else {
                sun.type = SpectralLight::Type::Distant;
                sun.direction = sunDir;
            }
            sun.color = GfVec3f(float(sunR * si), float(sunG * si), float(sunB * si));
            sun.intensity = 1.f;
            _scene->AddLight(sun);
        }

        // Sky dome fill
        double ski = _skyIntensity * m;
        if (ski > 0.001) {
            SpectralLight sky;
            sky.type = SpectralLight::Type::Dome;
            sky.color = GfVec3f(float(skyR * ski), float(skyG * ski), float(skyB * ski));
            sky.intensity = 1.f;
            _scene->AddLight(sky);
        }
    }

    // Studio three-point rig
    if (_studioPreset > 0 && _studioMix > 0.001) {
        double m = _studioMix;
        double ki = _studioKeyIntensity * m;

        // Key light (slightly warm) — sphere for soft shadows
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
            key.intensity = 1.f;
            _scene->AddLight(key);
        }

        // Fill light (opposite side, cool)
        double fi = ki * _studioFillRatio;
        if (fi > 0.001) {
            SpectralLight fill;
            fill.type = SpectralLight::Type::Distant;
            fill.direction = dirFromElevAzim(_studioKeyElevation * 0.5, _studioKeyAzimuth + 180);
            fill.color = GfVec3f(float(fi * 0.9), float(fi * 0.92), float(fi));
            fill.intensity = 1.f;
            _scene->AddLight(fill);
        }

        // Rim light (behind, high)
        double ri = _studioRimIntensity * m;
        if (ri > 0.001) {
            SpectralLight rim;
            rim.type = SpectralLight::Type::Distant;
            rim.direction = dirFromElevAzim(_studioKeyElevation + 15, _studioKeyAzimuth + 160);
            rim.color = GfVec3f(float(ri));
            rim.intensity = 1.f;
            _scene->AddLight(rim);
        }
    }
}

void SpectralRenderIop::_EnsureFrameRendered()
{
    // Allow re-entry for progressive refinement
    if (_frameReady.load()) {
        if (!_progressive || _progressiveSppDone >= _samples) return;
        // Progressive: preview done, need full quality
        _frameReady.store(false);
    }
    std::lock_guard<std::mutex> lock(_renderMutex);
    if (_frameReady.load()) return;

    // Load volume — upgrade to full 128³ for render if currently at 64³ preview
    _LoadVDB();          // ensures volume is loaded (may be 64³ preview)
    _LoadVDBForRender(); // upgrades to 128³ if needed

    // Skip if no scene content (no geometry AND no volume)
    bool hasGeometry = _scene && _scene->TotalTriangles() > 0;
    bool hasVolume = _volume && _volume->IsValid();
    if (!hasGeometry && !hasVolume) {
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
    if (_aovNormals)  _normalBuffer.assign(size_t(W) * H * 3, 0.f); else _normalBuffer.clear();
    if (_aovPosition) _posBuffer.assign(size_t(W) * H * 3, 0.f);    else _posBuffer.clear();
    if (_aovPRef)     _pRefBuffer.assign(size_t(W) * H * 3, 0.f);   else _pRefBuffer.clear();
    if (_aovUV)       _uvBuffer.assign(size_t(W) * H * 2, 0.f);     else _uvBuffer.clear();
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
    cam.fStop = _fStop;
    cam.focusDistance = _focusDistance;

    // Progressive rendering: first pass is a fast preview
    int renderSpp = _samples;
    if (_progressive && _progressiveSppDone == 0 && _samples > 8) {
        renderSpp = 8;  // fast preview pass
    }

    // Determine render device
    bool useGPU = false;
#ifdef SPECTRAL_HAS_OPTIX
    if (_deviceMode == 1) {          // gpu
        useGPU = true;
    } else if (_deviceMode == 2) {   // auto
        useGPU = SpectralIntegrator::IsGPUAvailable();
    }
#endif

    const char* deviceStr = useGPU ? "GPU" : "CPU";
    const char* passStr = (_progressive && renderSpp < _samples) ? " [preview]" : "";
    if (W != fullW || H != fullH)
        fprintf(stderr, "SpectralRender: rendering %dx%d (proxy of %dx%d), %zu tris, %d spp, device=%s%s\n",
                W, H, fullW, fullH, _scene->TotalTriangles(), renderSpp, deviceStr, passStr);
    else
        fprintf(stderr, "SpectralRender: rendering %dx%d, %zu tris, %d spp, device=%s%s\n",
                W, H, _scene->TotalTriangles(), renderSpp, deviceStr, passStr);

    // Caustics disabled — photon mapping removed for stability.
    // Will be re-implemented with progressive photon mapping in Phase 8d.
    const pxr::SpectralPhotonMap* pmap = nullptr;

    // Add built-in sun/sky and studio lights
    _BuildLightRig();

    // If volume loaded but no lights at all, add a default dome
    // so the volume is visible (extinction + scatter need light)
    if (_volume && _volume->IsValid() && !_scene->HasLights()) {
        pxr::SpectralLight defaultDome;
        defaultDome.type = pxr::SpectralLight::Type::Dome;
        defaultDome.color = pxr::GfVec3f(0.5f);
        defaultDome.intensity = 1.f;
        _scene->AddLight(defaultDome);
        fprintf(stderr, "SpectralRender: added default dome light for volume (enable Lighting tab for better results)\n");
    }

    // Volume already loaded at top of _EnsureFrameRendered

#ifdef SPECTRAL_HAS_OPTIX
    if (useGPU) {
        SpectralIntegrator::RenderFrameGPU(*_scene, cam, _frameBuffer.data(),
                                            renderSpp, _depthBuffer.data(), _maxBounces,
                                            _colorSpace, _volume.get());
        // Note: GPU caustics use CPU gathering pass below
    } else
#endif
    {
        SpectralIntegrator::AOVBuffers aovBufs;
        aovBufs.normal   = _aovNormals  ? _normalBuffer.data()   : nullptr;
        aovBufs.position = _aovPosition ? _posBuffer.data()      : nullptr;
        aovBufs.pRef     = _aovPRef     ? _pRefBuffer.data()     : nullptr;
        aovBufs.uv       = _aovUV       ? _uvBuffer.data()       : nullptr;
        aovBufs.albedo   = _aovAlbedo   ? _albedoBuffer.data()   : nullptr;
        aovBufs.direct   = _aovDirect   ? _directBuffer.data()   : nullptr;
        aovBufs.indirect = _aovIndirect ? _indirectBuffer.data() : nullptr;
        aovBufs.emission = _aovEmission ? _emissionBuffer.data() : nullptr;
        aovBufs.diffuseDirect   = _aovDiffuseDirect   ? _diffuseDirectBuffer.data()   : nullptr;
        aovBufs.specularDirect  = _aovSpecularDirect  ? _specularDirectBuffer.data()  : nullptr;
        aovBufs.diffuseIndirect = _aovDiffuseIndirect ? _diffuseIndirectBuffer.data() : nullptr;
        aovBufs.specularIndirect = _aovSpecularIndirect ? _specularIndirectBuffer.data() : nullptr;
        aovBufs.transmission    = _aovTransmission    ? _transmissionBuffer.data()    : nullptr;

        SpectralIntegrator::RenderFrame(*_scene, cam, _frameBuffer.data(),
                                         renderSpp, _depthBuffer.data(), _maxBounces,
                                         _objectIdBuffer.data(), _materialIdBuffer.data(),
                                         &aovBufs, nullptr, pmap, _causticRadius,
                                         _colorSpace, _volume.get());
    }

    // Denoise — works for both GPU and CPU renders
#ifdef SPECTRAL_HAS_OPTIX
    if (_denoise) {
        SpectralIntegrator::DenoiseGPU(W, H, _frameBuffer.data());
    }
#endif

    // Geometry AOV pass for GPU (CPU fills them during RenderFrame)
#ifdef SPECTRAL_HAS_OPTIX
    if (useGPU) {
        bool needGeomPass = _aovNormals || _aovPosition || _aovPRef || _aovUV || _aovAlbedo;
        if (needGeomPass) {
            SpectralIntegrator::ComputeGeometryAOVs(
                *_scene, cam,
                _aovNormals  ? _normalBuffer.data() : nullptr,
                _aovPosition ? _posBuffer.data()    : nullptr,
                _aovPRef     ? _pRefBuffer.data()   : nullptr,
                _aovUV       ? _uvBuffer.data()     : nullptr,
                _aovAlbedo   ? _albedoBuffer.data() : nullptr,
                _objectIdBuffer.data(), _materialIdBuffer.data(),
                nullptr);
        }

        // Shading AOV pass: quick CPU render for direct/indirect/emission/LPE
        bool needShadingPass = _aovDirect || _aovIndirect || _aovEmission ||
                               _aovDiffuseDirect || _aovSpecularDirect ||
                               _aovDiffuseIndirect || _aovSpecularIndirect ||
                               _aovTransmission;
        if (needShadingPass) {
            const int aovSpp = std::min(8, renderSpp);
            std::vector<float> dummyPixels(size_t(W) * H * 4, 0.f);

            SpectralIntegrator::AOVBuffers aovBufs;
            aovBufs.direct   = _aovDirect   ? _directBuffer.data()   : nullptr;
            aovBufs.indirect = _aovIndirect ? _indirectBuffer.data() : nullptr;
            aovBufs.emission = _aovEmission ? _emissionBuffer.data() : nullptr;
            aovBufs.diffuseDirect   = _aovDiffuseDirect   ? _diffuseDirectBuffer.data()   : nullptr;
            aovBufs.specularDirect  = _aovSpecularDirect  ? _specularDirectBuffer.data()  : nullptr;
            aovBufs.diffuseIndirect = _aovDiffuseIndirect ? _diffuseIndirectBuffer.data() : nullptr;
            aovBufs.specularIndirect = _aovSpecularIndirect ? _specularIndirectBuffer.data() : nullptr;
            aovBufs.transmission    = _aovTransmission    ? _transmissionBuffer.data()    : nullptr;

            SpectralIntegrator::RenderFrame(*_scene, cam, dummyPixels.data(),
                                             aovSpp, nullptr, _maxBounces,
                                             nullptr, nullptr, &aovBufs, nullptr,
                                             pmap, _causticRadius, _colorSpace,
                                             _volume.get());

            fprintf(stderr, "SpectralRender: shading AOVs computed (%d spp CPU pass)\n", aovSpp);
        }
    }
#endif

    // AO pass
    if (_aoSamples > 0) {
        SpectralIntegrator::ComputeAO(*_scene, cam, _aoBuffer.data(),
                                       _aoSamples, _aoRadius);
    }

    _progressiveSppDone = renderSpp;

    // Generate Cryptomatte from objectId buffer
    // Nuke Cryptomatte spec: crypto_object00 RGBA = (id0_hash, coverage0, id1_hash, coverage1)
    if (!_cryptoObjectBuffer.empty() && !_objectIdBuffer.empty()) {
        for (size_t i = 0; i < size_t(W) * H; ++i) {
            float objId = _objectIdBuffer[i];
            // MurmurHash3-style bit mixing for stable float ID
            uint32_t h = static_cast<uint32_t>(objId);
            h ^= h >> 16; h *= 0x85ebca6b;
            h ^= h >> 13; h *= 0xc2b2ae35;
            h ^= h >> 16;
            // Ensure non-NaN/Inf by clearing exponent overflow
            h &= 0x7fffffff; if ((h & 0x7f800000) == 0x7f800000) h = 0;
            float hashF;
            memcpy(&hashF, &h, sizeof(float));
            _cryptoObjectBuffer[i * 4 + 0] = hashF;           // R: id hash
            _cryptoObjectBuffer[i * 4 + 1] = (objId > 0.f) ? 1.f : 0.f;  // G: coverage
            _cryptoObjectBuffer[i * 4 + 2] = 0.f;             // B: second id (none)
            _cryptoObjectBuffer[i * 4 + 3] = 0.f;             // A: second coverage
        }
    }

    // If this was a preview pass, schedule refinement
    if (_progressive && _progressiveSppDone < _samples) {
        fprintf(stderr, "SpectralRender: preview complete (%d spp) — re-render for full %d spp\n",
                _progressiveSppDone, _samples);
    }

    _frameReady.store(true);

    fprintf(stderr, "SpectralRender: render complete\n");
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
    _EnsureFrameRendered();

    const int W = static_cast<int>(_fbWidth);
    const int H = static_cast<int>(_fbHeight);

    if (_frameBuffer.empty() || _depthBuffer.empty() || W == 0 || H == 0)
        return true;  // empty plane

    // Create the output plane with requested channels
    DD::Image::ChannelSet outChans = channels;
    outChans += Chan_DeepFront;
    outChans += Chan_DeepBack;

    plane = DeepOutputPlane(outChans, box, DeepPixel::eZAscending);

    const int nChans = outChans.size();

    for (DD::Image::Box::iterator it = box.begin(); it != box.end(); ++it) {
        const int px = it.x;
        const int py = it.y;

        // Flip Y: Nuke bottom-up, our buffer top-down
        const int bufY = H - 1 - py;

        if (px < 0 || px >= W || bufY < 0 || bufY >= H) {
            plane.addHole();
            continue;
        }

        const size_t pixIdx = static_cast<size_t>(bufY) * W + px;
        const float depth = _depthBuffer[pixIdx];

        // No hit = no deep sample (sky pixels)
        if (depth >= 1e29f) {
            plane.addHole();
            continue;
        }

        const float* rgba = _frameBuffer.data() + pixIdx * 4;

        // One deep sample per pixel
        DeepOutPixel op(nChans);
        foreach(z, outChans) {
            if      (z == Chan_DeepFront) op.push_back(depth);
            else if (z == Chan_DeepBack)  op.push_back(depth + 0.001f);
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
