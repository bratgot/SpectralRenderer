#include "SpectralRenderIop.h"
#include "SpectralSurfaceOp.h"
#include <chrono>

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
            Double_knob(f, &_skyIntensity, "sky_intensity", "sky fill"); SetRange(f, 0, 20);
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
        Tooltip(f, "Lit \xe2\x80\x94 full lighting, shadows, phase function\n"
                   "Greyscale \xe2\x80\x94 density preview, no lighting (fastest)\n"
                   "Heat \xe2\x80\x94 warm ramp: black \xe2\x86\x92 red \xe2\x86\x92 yellow \xe2\x86\x92 white\n"
                   "Cool \xe2\x80\x94 cool ramp: black \xe2\x86\x92 blue \xe2\x86\x92 cyan \xe2\x86\x92 white\n"
                   "Blackbody \xe2\x80\x94 temperature to fire colour (RGB)\n"
                   "Explosion \xe2\x80\x94 lit smoke + self-luminous fire");
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

        // ─── Viewport ───────────────────────────────────────────────────
        Divider(f, "3D Viewport");
        Bool_knob(f, &_vdb3dPreview, "vdb_3d_preview", "3D preview");
        Tooltip(f, "Draw volume bbox and point cloud in the 3D viewer.\n"
                   "Uses the same render mode coloring as the final render.\n"
                   "Transform follows GeoTransform from the scene graph.");
        Bool_knob(f, &_vdbShowBbox, "vdb_show_bbox", "bbox");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Green wireframe bounding box in the 3D viewer.\n"
                   "Useful for camera placement before rendering.");
        Bool_knob(f, &_vdbShowPoints, "vdb_show_points", "points");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Coloured point cloud preview.\n"
                   "Colour scheme follows the current Render Mode.");
        Double_knob(f, &_vdbPointDensity, "vdb_point_density", "density");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 0.1, 1.0);
        Tooltip(f, "Point sampling: 0.1 = sparse/fast, 1.0 = all voxels.");
        Double_knob(f, &_vdbPointSize, "vdb_point_size", "size");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 1, 8);
        Tooltip(f, "GL point size in pixels.");
        Newline(f);
        Bool_knob(f, &_vdbFastScrub, "vdb_fast_scrub", "fast scrub");
        Tooltip(f, "Bbox-only during timeline scrub.\n"
                   "Full point cloud loads when playback stops.");
        Bool_knob(f, &_vdbCacheEnabled, "vdb_cache_enabled", "cache");
        ClearFlags(f, Knob::STARTLINE);
        Int_knob(f, &_vdbCacheMax, "vdb_cache_max", "");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 1, 32);
        Text_knob(f, "frames");
        ClearFlags(f, Knob::STARTLINE);
        Button(f, "vdb_cache_clear", "Clear Cache");
        ClearFlags(f, Knob::STARTLINE);

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

    // ─── Motion Blur ────────────────────────────────────────────────
    Divider(f, "Motion blur");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Velocity-based motion blur from VDB velocity grids (vel/v).<br>"
        "Set the velocity grid in the Volumes tab Grids section."
        "</font>"
    );
    Newline(f);
    Bool_knob(f, &_motionBlur, "motion_blur", "enable");
    Tooltip(f, "Render motion blur from velocity grid data.\n"
               "Offsets ray origins across the shutter interval.\n"
               "Requires a velocity grid to be loaded.");
    static const char* const shutP[] = {
        "Start (0 to 1)", "Centre (-0.5 to 0.5)", "End (-1 to 0)", "Custom", nullptr
    };
    Enumeration_knob(f, &_shutterPreset, shutP, "shutter_preset", "shutter");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Start = blur trails behind motion direction\n"
               "Centre = blur centred on current frame (default)\n"
               "End = blur leads ahead of motion\n"
               "Custom = set open/close manually");
    Newline(f);
    Double_knob(f, &_shutterOpen, "shutter_open", "open"); SetRange(f, -1, 0);
    Tooltip(f, "Shutter open time relative to frame.\n"
               "-0.5 = half frame before current.");
    Double_knob(f, &_shutterClose, "shutter_close", "close"); SetRange(f, 0, 1);
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Shutter close time relative to frame.\n"
               "0.5 = half frame after current.");
    Int_knob(f, &_motionSamples, "motion_samples", "samples"); SetRange(f, 2, 8);
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Time samples across shutter. 2 = fast. 3 = good. 5 = smooth.");

    // ─── Camera Motion Blur (TODO) ─────────────────────────────────
    Bool_knob(f, &_cameraMblur, "camera_mblur", "camera blur");
    SetFlags(f, Knob::INVISIBLE);
    Int_knob(f, &_cameraMblurQuality, "camera_mblur_quality", "quality");
    SetFlags(f, Knob::INVISIBLE);

}

int SpectralRenderIop::knob_changed(Knob* k)
{
    _frameReady.store(false);
    _progressiveSppDone = 0;

    // Update DAG node label with CPU/GPU indicator
    if (k->is("device_mode") || k->is("showPanel")) {
        // DAG node appearance — colored tile + label showing active device
        int idx = std::max(0, std::min(_deviceMode, 2));

        // Tile color: CPU=blue tint, GPU=green tint, Auto=amber tint
        unsigned int tileColors[] = { 0x1a3a5aFF, 0x1a4a2aFF, 0x3a3a1aFF };
        std::string nm(node_name());
        std::string cmd = "import nuke; n = nuke.toNode('" + nm + "')\n"
                          "if n: n['tile_color'].setValue(" + std::to_string(tileColors[idx]) + ")";
        script_command(cmd.c_str(), true, false);

        // Label with colored device name
        const char* labels[] = {
            "<center><font color='#4499dd' size='2'><b>CPU</b></font></center>",
            "<center><font color='#44dd88' size='2'><b>GPU</b></font></center>",
            "<center><font color='#ddaa44' size='2'><b>AUTO</b></font></center>"
        };
        if (Knob* lk = knob("label")) lk->set_text(labels[idx]);
        if (k->is("device_mode")) return 1;
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
        _frameReady.store(false);
        _vdbPreviewDirty = true; _vdbPreviewPoints.clear();
        _vdbLoadedPath.clear();
        _volume.reset();  // force reload
        // Re-detect sequence if auto is on
        if (_vdbAutoSequence && k->is("vdb_file")) {
            if (Knob* ok = knob("vdb_orig_file")) ok->set_text(_vdbFile ? _vdbFile : "");
        }
        return 1;
    }
    if (k->is("vdb_3d_preview") || k->is("vdb_show_points") || k->is("vdb_point_density") || k->is("vdb_point_size") || k->is("vdb_show_bbox") || k->is("vdb_fast_scrub")) {
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
        _frameReady.store(false);
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
        _vdbLastLoadedFrame = -999;
        _volume.reset();
        _volumes.clear();
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
        if (newHash != _scnInputHash) {
            _scnInputHash = newHash;
            _vdbLastLoadedFrame = -999;  // force VDB reload
            _frameReady.store(false);    // force re-render
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
        _frameReady.store(false);
        _vdbPreviewDirty = true;
        _vdbPreviewPoints.clear();
        scnChanged = true;
    }

    if (forReal || scnChanged) {
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

            // Build a usg::Stage from the upstream GeomOp
            usg::StageRef usgStage = usg::Stage::CreateInMemory();
            if (!usgStage) {
                SLOG("SpectralRender: failed to create in-memory stage\n");
                return;
            }

            // Use the current frame time
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

            SLOG("SpectralRender: got PXR stage from node graph\n");
            _LoadFromPxrStage(*pxrStagePtr);
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

                    matId = _scene->AddMaterial(mat);

                    SLOG("SpectralRender: material '%s' — color=(%.2f,%.2f,%.2f) metal=%.2f rough=%.2f opacity=%.2f\n",
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
                SLOG("SpectralRender: auto-subdividing %s for displacement\n",
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

                    SLOG("SpectralRender: mesh %s subdivided (%s level 2)\n",
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
    hash.append(outputContext().frame());
    hash.append(_vdbFrameOffset);
    if (_vdbFile) hash.append(_vdbFile);
    hash.append((int)_volumes.size());
    // Hash scn input connection + disabled state
    Op* scn = (inputs() > 1) ? input(1) : nullptr;
    if (scn) {
        hash.append(scn->hash());
        hash.append(scn->node_disabled() ? 1 : 0);
    } else {
        hash.append(0);
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

    // Local GL preview (point cloud + bbox + env light dome)
    if (_vdb3dPreview) {
        bool hasVolume = !_volumes.empty() || (_volume && _volume->HasBbox());
        bool hasLights = _cachedEnvLight || _cachedStudioLight;
        if (hasVolume || hasLights) add_draw_handle(ctx);
    }

    // Camera handles
    Op* cam = (inputs() > 0) ? input(0) : nullptr;
    if (cam) cam->build_handles(ctx);
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

            int totalBudget = 20000;  // total points across all volumes
            int maxPointsPerVol = std::max(500, totalBudget / std::max(1, (int)_volumes.size()));
            for (size_t vj = 0; vj < _volumes.size(); ++vj) {
                auto& v = _volumes[vj];
                if (!v || !v->IsValid()) continue;
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
                            if (d < 0.01f) continue;
                            if (d > _vdbMaxDensity) _vdbMaxDensity = d;
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
                            pt.density = d;
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
            float inv = 1.f / _vdbMaxDensity;
            glBegin(GL_POINTS);
            for (const auto& pt : _vdbPreviewPoints) {
                float t = std::min(pt.density * inv, 1.f);
                float r, g, b;
                switch (_vdbRenderMode) {
                    case 1: r = g = b = t; break;
                    case 3: r=std::max(0.f,std::min((t-.5f)*2.f,1.f)); g=std::max(0.f,std::min((t-.25f)*2.f,1.f)); b=std::min(t*2.f,1.f); break;
                    case 4: r=std::min(t*2.f,1.f); g=std::max(0.f,std::min((t-.3f)*2.f,1.f)); b=std::max(0.f,std::min((t-.7f)*3.f,1.f)); break;
                    case 2: r=std::min(t*3.f,1.f); g=std::max(0.f,std::min((t-.33f)*3.f,1.f)); b=std::max(0.f,std::min((t-.66f)*3.f,1.f)); break;
                    default: r=t; g=t*0.9f; b=t*0.7f; break;
                }
                glColor4f(r, g, b, .15f+.85f*t);
                glVertex3f(pt.x, pt.y, pt.z);
            }
            glEnd(); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }

    // ─── Light preview (subtle, professional) ────────────────────────

    // Environment light
    if (_cachedEnvLight && _cachedEnvLight->skyPreset > 0) {
        float R = float(_cachedEnvLight->domeRadius);
        if (_cachedEnvLight->showDome) {
            // Ground horizon circle — bright
            glColor4f(0.5f, 0.6f, 1.0f, 0.8f); glLineWidth(2.f);
            glBegin(GL_LINE_LOOP);
            for (int i=0;i<64;++i){float a=float(i)/64.f*2.f*kPi;glVertex3f(R*std::cos(a),0,R*std::sin(a));}
            glEnd();
            // 45deg latitude
            glColor4f(0.4f, 0.6f, 1.0f, 0.6f); glLineWidth(1.5f);
            float elR45 = 45.f*kPi/180.f;
            float rr45 = R*std::cos(elR45), y45 = R*std::sin(elR45);
            glBegin(GL_LINE_LOOP);
            for (int i=0;i<48;++i){float a=float(i)/48.f*2.f*kPi;glVertex3f(rr45*std::cos(a),y45,rr45*std::sin(a));}
            glEnd();
            // 30deg latitude
            float elR30 = 30.f*kPi/180.f;
            float rr30 = R*std::cos(elR30), y30 = R*std::sin(elR30);
            glColor4f(0.4f, 0.6f, 1.0f, 0.4f);
            glBegin(GL_LINE_LOOP);
            for (int i=0;i<48;++i){float a=float(i)/48.f*2.f*kPi;glVertex3f(rr30*std::cos(a),y30,rr30*std::sin(a));}
            glEnd();
            // Zenith cross
            glColor4f(0.4f, 0.6f, 1.0f, 0.3f); glLineWidth(1.f);
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
            float sx=R*std::cos(elR)*std::sin(azR), sy=R*std::sin(elR), sz=R*std::cos(elR)*std::cos(azR);
            // Thin line from origin to sun
            glColor4f(1.f, 0.85f, 0.4f, 0.15f+sunI*0.2f); glLineWidth(1.f);
            glBegin(GL_LINES);glVertex3f(0,0,0);glVertex3f(sx,sy,sz);glEnd();
            // Clean sun dot — size from intensity
            float sunSz = 4.f + sunI * 8.f;
            glColor4f(1.f, 0.9f, 0.5f, 0.5f+sunI*0.4f); glPointSize(sunSz);
            glBegin(GL_POINTS);glVertex3f(sx,sy,sz);glEnd();
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
            int previewMaxRes = 32;
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
            int previewMaxRes = 32;  // preview only — render res loaded in _EnsureFrameRendered

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
            double si = sunPow * std::max(0.1, std::min(el->sunElevation / 12.0, 1.0)) * skyMix;
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
                sun.intensity = 1.f;
                _scene->AddLight(sun);
                SLOG("SpectralRender: env sky sun type=%s softness=%.2f radius=%.1f\n",
                    sun.type == SpectralLight::Type::Sphere ? "Sphere" : "Distant",
                    el->sunShadowSoftness, sun.radius);
            }

            double skyPow = el->skyIntensity * el->skyIntensity;
            double ski = skyPow * skyMix;
            if (ski > 0.001) {
                SpectralLight sky;
                sky.type = SpectralLight::Type::Dome;
                sky.color = GfVec3f(float(skyR * ski), float(skyG * ski), float(skyB * ski));
                sky.intensity = 1.f;

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
            rim.intensity = 1.f;
            _scene->AddLight(rim);
        }
    }

    // ---- Fallback: use SpectralRender's own lighting knobs if no light nodes connected ----
    if (_allEnvLights.empty() && _allStudioLights.empty()) {
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
                sun.intensity = 1.f;
                _scene->AddLight(sun);
            }

            double skyPow = _skyIntensity * _skyIntensity;
            double ski = skyPow * m;
            if (ski > 0.001) {
                SpectralLight sky;
                sky.type = SpectralLight::Type::Dome;
                sky.color = GfVec3f(float(skyR * ski), float(skyG * ski), float(skyB * ski));
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
                key.intensity = 1.f;
                _scene->AddLight(key);
            }
            double fi = ki * _studioFillRatio;
            if (fi > 0.001) {
                SpectralLight fill;
                fill.type = SpectralLight::Type::Distant;
                fill.direction = dirFromElevAzim(_studioKeyElevation * 0.5, _studioKeyAzimuth + 180);
                fill.color = GfVec3f(float(fi * 0.9), float(fi * 0.92), float(fi));
                fill.intensity = 1.f;
                _scene->AddLight(fill);
            }
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
    // Allow re-entry for progressive refinement
    if (_frameReady.load()) {
        if (!_progressive || _progressiveSppDone >= _samples) return;
        // Progressive: preview done, need full quality
        _frameReady.store(false);
    }
    std::lock_guard<std::mutex> lock(_renderMutex);
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
    // Camera motion blur: multiply samples for quality
    if (_cameraMblur && _camera.cameraMblur) {
        renderSpp = std::max(renderSpp, renderSpp * _cameraMblurQuality / 4);
    }
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

    auto tGpuStart = std::chrono::high_resolution_clock::now();

#ifdef SPECTRAL_HAS_OPTIX
    if (useGPU) {
        SpectralIntegrator::RenderFrameGPU(*_scene, cam, _frameBuffer.data(),
                                            renderSpp, _depthBuffer.data(), _maxBounces,
                                            _colorSpace,
                                            volPtrs.empty() ? nullptr : volPtrs.data(),
                                            (int)volPtrs.size());
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
                                         _colorSpace,
                                         volPtrs.empty() ? nullptr : volPtrs.data(),
                                         (int)volPtrs.size());
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
                                             volPtrs.empty() ? nullptr : volPtrs.data(),
                                             (int)volPtrs.size());

            SLOG("SpectralRender: shading AOVs computed (%d spp CPU pass)\n", aovSpp);
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
        SLOG("SpectralRender: preview complete (%d spp) — re-render for full %d spp\n",
                _progressiveSppDone, _samples);
    }

    _frameReady.store(true);

    {
        auto tEnd = std::chrono::high_resolution_clock::now();
        auto msGpu = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tGpuStart).count();
        auto msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tEnsureStart).count();
        SLOG("SpectralRender: render complete (render=%lldms total=%lldms)\n", msGpu, msTotal);
    }
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
