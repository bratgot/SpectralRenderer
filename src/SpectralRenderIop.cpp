#include "SpectralRenderIop.h"
#include "SpectralSurfaceOp.h"

#ifdef SPECTRAL_HAS_OSD
#include "SpectralSubdiv.h"
#endif

// PXR — USD stage traversal
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdShade/material.h>
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
}

SpectralRenderIop::~SpectralRenderIop() = default;

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
        "If no scene input is connected, set the 'USD file' knob\n"
        "to a .usd/.usda/.usdc file path instead.\n\n"
        "For interactive viewport, use the Hydra delegate\n"
        "(\"Spectral (CPU)\" in the renderer dropdown).";
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
const char* SpectralRenderIop::input_label(int idx, char*) const
{
    switch (idx) {
        case 0: return "Scene";
        case 1: return "Cam";
        case 2: return "BG";
        default: return "";
    }
}

bool SpectralRenderIop::test_input(int idx, Op* op) const
{
    if (!op) return true;  // allow disconnection on any input

    switch (idx) {
        case 0: {
            // Accept any Op that implements GeometryProviderI
            GeometryProviderI* gp = dynamic_cast<GeometryProviderI*>(op);
            return (gp != nullptr);
        }
        case 1: {
            // Accept CameraOp (covers both classic and CameraSceneOp)
            return dynamic_cast<CameraOp*>(op) != nullptr;
        }
        case 2: {
            // Accept any Iop for background
            return dynamic_cast<Iop*>(op) != nullptr;
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
        Enumeration_knob(f, &_deviceMode, deviceNames, "device", "device");
        Tooltip(f, "Rendering device:<br>"
                   "cpu = Embree 4 CPU ray tracing<br>"
                   "gpu = OptiX 8.1 RTX GPU ray tracing<br>"
                   "auto = GPU if available, otherwise CPU");
        static const char* const proxyNames[] = { "1/4", "1/2", "3/4", "full", nullptr };
        Enumeration_knob(f, &_proxyMode, proxyNames, "proxy", "proxy");
        Tooltip(f, "Render resolution proxy.<br>"
                   "1/4 = quarter resolution (fastest preview)<br>"
                   "1/2 = half resolution<br>"
                   "3/4 = three-quarter resolution<br>"
                   "full = full output resolution");
        Int_knob(f, &_samples, "samples", "samples per pixel"); SetRange(f, 1, 256);
        Tooltip(f, "Number of spectral samples per pixel.<br>"
                   "Higher values reduce noise. Each sample<br>"
                   "traces one wavelength through the scene.<br>"
                   "1 = normal-shaded preview, 16+ = spectral.");
        Int_knob(f, &_maxBounces, "max_bounces", "max bounces"); SetRange(f, 1, 16);
        Tooltip(f, "Maximum number of light bounces per ray.<br>"
                   "1 = direct lighting only<br>"
                   "4 = good for most scenes (default)<br>"
                   "8+ = needed for complex glass and caustics");
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

    BeginClosedGroup(f, "aov_grp", "AOV outputs");
    {
        Int_knob(f, &_aoSamples, "ao_samples", "AO samples"); SetRange(f, 0, 64);
        Tooltip(f, "Ambient occlusion samples per pixel.<br>"
                   "0 = disabled (no AO output channel)<br>"
                   "8-16 = good quality<br>"
                   "32+ = clean result<br>"
                   "Output written to the other.ao channel.");
        Float_knob(f, &_aoRadius, "ao_radius", "AO radius"); SetRange(f, 0.1f, 100.f);
        Tooltip(f, "Maximum distance for ambient occlusion rays.<br>"
                   "Smaller values give tighter contact shadows.<br>"
                   "Larger values give broader ambient darkening.");
        Newline(f);
        Bool_knob(f, &_aovNormals, "aov_normals", "N");
        Tooltip(f, "Output world-space surface normals<br>"
                   "to the N.red, N.green, N.blue channels.");
        Bool_knob(f, &_aovPosition, "aov_position", "P");
        Tooltip(f, "Output world-space hit position<br>"
                   "to the P.red, P.green, P.blue channels.");
        Bool_knob(f, &_aovPRef, "aov_pref", "Pref");
        Tooltip(f, "Reference position in object/local space.<br>"
                   "Stays constant across animation and deformation.<br>"
                   "Used for sticky texture projections and mattes<br>"
                   "that follow deforming or animated geometry.");
        Bool_knob(f, &_aovUV, "aov_uv", "UV");
        Tooltip(f, "Output texture coordinates to<br>"
                   "the uv.red and uv.green channels.");
        Bool_knob(f, &_aovAlbedo, "aov_albedo", "albedo");
        Tooltip(f, "Output resolved surface base colour<br>"
                   "(with textures applied) to the albedo layer.");
        Bool_knob(f, &_aovDirect, "aov_direct", "direct");
        Tooltip(f, "Output direct lighting component to<br>"
                   "the direct layer. On GPU renders this<br>"
                   "requires an extra CPU pass at low spp.");
        Bool_knob(f, &_aovIndirect, "aov_indirect", "indirect");
        Tooltip(f, "Output indirect/bounce lighting component<br>"
                   "to the indirect layer. On GPU renders this<br>"
                   "requires an extra CPU pass at low spp.");
        Bool_knob(f, &_aovEmission, "aov_emission", "emission");
        Tooltip(f, "Output emissive surface contribution<br>"
                   "to the emission layer. On GPU renders this<br>"
                   "requires an extra CPU pass at low spp.");
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

    // Update node label in DAG
    const char* device = "CPU";
#ifdef SPECTRAL_HAS_OPTIX
    if (_deviceMode == 1) device = "GPU";
    else if (_deviceMode == 2) device = "auto";
#endif
    if (Knob* lk = knob("label")) {
        char buf[128];
        static const char* const proxyLabels[] = { "1/4", "1/2", "3/4", "full" };
        const char* proxyStr = (_proxyMode >= 0 && _proxyMode <= 3) ? proxyLabels[_proxyMode] : "full";
        snprintf(buf, sizeof(buf), "%s\n%d spp\n%d bounces\n%s",
                 device, _samples, _maxBounces, proxyStr);
        lk->set_text(buf);
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
    info_.channels(channels);
    info_.black_outside(true);

    if (forReal) {
        _LoadStage();
        _BuildCameraFromInput();
        _frameReady.store(false);
        _progressiveSppDone = 0;
    }

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

        // Process meshes
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
                                if (mat.bumpMapTexId < 0 && entry.second.bumpIop) {
                                    Iop* bumpIop = dynamic_cast<Iop*>(entry.second.bumpIop);
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
                                        return texPx[idx]; // use red channel
                                    };

                                    float c00 = samplePx(x0,y0), c10 = samplePx(x1,y0);
                                    float c01 = samplePx(x0,y1), c11 = samplePx(x1,y1);
                                    float val = (c00*(1-dx)+c10*dx)*(1-dy) + (c01*(1-dx)+c11*dx)*dy;

                                    float offset = (val - midpoint) * scale;
                                    GfVec3f N = normals[i];
                                    float nlen = N.GetLength();
                                    if (nlen > 1e-6f) N /= nlen;
                                    points[i] += N * offset;
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

    // Skip if no scene loaded or empty
    if (!_scene || _scene->TotalTriangles() == 0) {
        _frameReady.store(true);
        return;
    }

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

#ifdef SPECTRAL_HAS_OPTIX
    if (useGPU) {
        SpectralIntegrator::RenderFrameGPU(*_scene, cam, _frameBuffer.data(),
                                            renderSpp, _depthBuffer.data(), _maxBounces);
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

        SpectralIntegrator::RenderFrame(*_scene, cam, _frameBuffer.data(),
                                         renderSpp, _depthBuffer.data(), _maxBounces,
                                         _objectIdBuffer.data(), _materialIdBuffer.data(),
                                         &aovBufs, nullptr, pmap, _causticRadius);
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

        // Shading AOV pass: quick CPU render for direct/indirect/emission
        bool needShadingPass = _aovDirect || _aovIndirect || _aovEmission;
        if (needShadingPass) {
            const int aovSpp = std::min(8, renderSpp);
            std::vector<float> dummyPixels(size_t(W) * H * 4, 0.f);

            SpectralIntegrator::AOVBuffers aovBufs;
            aovBufs.direct   = _aovDirect   ? _directBuffer.data()   : nullptr;
            aovBufs.indirect = _aovIndirect ? _indirectBuffer.data() : nullptr;
            aovBufs.emission = _aovEmission ? _emissionBuffer.data() : nullptr;

            SpectralIntegrator::RenderFrame(*_scene, cam, dummyPixels.data(),
                                             aovSpp, nullptr, _maxBounces,
                                             nullptr, nullptr, &aovBufs, nullptr,
                                             pmap, _causticRadius);

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
