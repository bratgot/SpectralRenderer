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
    // Input 2 (BG): provide a Black node as default so Nuke
    // doesn't complain about unconnected Iop inputs.
    // Inputs 0 and 1 are optional — return null.
    if (idx == 2) return Iop::default_input(0);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------
void SpectralRenderIop::knobs(Knob_Callback f)
{
    File_knob(f, &_usdFilePath, "usd_file", "USD file");
    Tooltip(f, "Path to a .usd/.usda/.usdc file.\n"
               "Only used when input 0 is not connected.");

    String_knob(f, &_cameraPath, "camera_path", "camera prim");
    Tooltip(f, "USD prim path e.g. /World/Camera. Leave blank to auto-find.");

    Int_knob(f, &_frame, "frame", "frame");

    Format_knob(f, &_outputFormat, "format", "format");

    Divider(f, "Render settings");
    static const char* const deviceNames[] = { "cpu", "gpu", "auto", nullptr };
    Enumeration_knob(f, &_deviceMode, deviceNames, "device", "device");
    Tooltip(f, "cpu: Embree CPU ray tracing\n"
               "gpu: OptiX RTX GPU ray tracing\n"
               "auto: GPU if available, else CPU");
    Int_knob(f, &_samples,    "samples",     "samples per pixel"); SetRange(f, 1, 256);
    Int_knob(f, &_maxBounces, "max_bounces", "max bounces");       SetRange(f, 1, 16);
    Int_knob(f, &_tileSize,   "tile_size",   "tile size");         SetRange(f, 16, 256);

    Divider(f, "Lighting");
    Float_knob(f, &_lightIntensity, "light_intensity", "light intensity");
    SetRange(f, 0.01f, 10.f);
    Tooltip(f, "Global light intensity multiplier.\n"
               "Nuke's USD pipeline uses physical light units which\n"
               "can produce very large values. Adjust this to\n"
               "compensate. Default 1.0.");

    static const char* const illuminantNames[] = {
        "auto", "D50", "D65", "A", "F2", "F11", nullptr
    };
    Enumeration_knob(f, &_illuminant, illuminantNames, "illuminant", "illuminant");
    Tooltip(f, "CIE illuminant override for all lights.\n"
               "auto = use each light's own color/temperature\n"
               "D50  = CIE D50 (horizon daylight, 5003K)\n"
               "D65  = CIE D65 (noon daylight, 6504K)\n"
               "A    = CIE A (tungsten incandescent, 2856K)\n"
               "F2   = CIE F2 (cool white fluorescent)\n"
               "F11  = CIE F11 (narrow-band fluorescent)");


    Divider(f, "Motion blur");
    Float_knob(f, &_shutterOpen,  "shutter_open",  "shutter open");  SetRange(f, -1.f, 0.f);
    Float_knob(f, &_shutterClose, "shutter_close", "shutter close"); SetRange(f, 0.f, 1.f);
    Tooltip(f, "Shutter interval relative to frame.\n"
               "-0.5 / 0.5 = centered on frame (180 degree shutter)\n"
               "0.0 / 0.5  = forward motion blur\n"
               "0.0 / 0.0  = no motion blur");
}

int SpectralRenderIop::knob_changed(Knob* k)
{
    _frameReady.store(false);
    return Iop::knob_changed(k);
}

// ---------------------------------------------------------------------------
// _validate
// ---------------------------------------------------------------------------
void SpectralRenderIop::_validate(bool forReal)
{
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
    channels += _chanObjectId;
    channels += _chanMaterialId;
    info_.channels(channels);
    info_.black_outside(true);

    if (forReal) {
        _LoadStage();
        _BuildCameraFromInput();
        _frameReady.store(false);
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

    const int W = static_cast<int>(_fbWidth);
    const int H = static_cast<int>(_fbHeight);
    int bufY = H - 1 - y;

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

    // Composite: render over BG using render alpha
    auto compositeChannel = [&](Channel ch, int comp) {
        if (!(channels & ch)) return;
        float* out = row.writable(ch);
        const float* bgPtr = hasBG ? bgRow[ch] + x : nullptr;

        for (int px = x; px < r; ++px) {
            float fg = 0.f, fgA = 0.f;
            if (px >= 0 && px < W) {
                fg  = srcRow[px * 4 + comp];
                fgA = srcRow[px * 4 + 3];
            }

            if (bgPtr) {
                // Standard "over" composite: fg + bg * (1 - fgA)
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
            float fgA = (px >= 0 && px < W) ? srcRow[px * 4 + 3] : 0.f;
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
            if (px >= 0 && px < W && bufY >= 0 && bufY < H) {
                out[px] = idBuf[static_cast<size_t>(bufY) * W + px];
            } else {
                out[px] = 0.f;
            }
        }
    };
    writeIdChannel(_chanObjectId, _objectIdBuffer);
    writeIdChannel(_chanMaterialId, _materialIdBuffer);
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
                light.radius = r;
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
                                if (mat.abbeNumber > 0.f || mat.thinFilmThickness > 0.f) {
                                    fprintf(stderr, "SpectralRender: spectral props from '%s'"
                                            " — Abbe=%.1f thinFilm=%.0fnm\n",
                                            entry.first.c_str(),
                                            mat.abbeNumber, mat.thinFilmThickness);
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

                    // UVs don't survive subdivision yet — clear them
                    // (OpenSubdiv UV refinement is a future enhancement)
                    uvs.clear();

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
    if (_frameReady.load()) return;
    std::lock_guard<std::mutex> lock(_renderMutex);
    if (_frameReady.load()) return;

    // Use the format from info_ (set by _validate from BG or format knob)
    const Format* fmtPtr = &(info_.format());
    if (!fmtPtr) { _frameReady.store(true); return; }
    const unsigned int W = static_cast<unsigned int>(fmtPtr->width());
    const unsigned int H = static_cast<unsigned int>(fmtPtr->height());
    if (W == 0 || H == 0) { _frameReady.store(true); return; }

    _fbWidth  = W;
    _fbHeight = H;
    _frameBuffer.assign(size_t(W) * H * 4, 0.f);
    _depthBuffer.assign(size_t(W) * H, 1e30f);
    _objectIdBuffer.assign(size_t(W) * H, 0.f);
    _materialIdBuffer.assign(size_t(W) * H, 0.f);

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
    fprintf(stderr, "SpectralRender: rendering %dx%d, %zu tris, %d spp, device=%s\n",
            W, H, _scene->TotalTriangles(), _samples, deviceStr);

#ifdef SPECTRAL_HAS_OPTIX
    if (useGPU) {
        SpectralIntegrator::RenderFrameGPU(*_scene, cam, _frameBuffer.data(),
                                            _samples, _depthBuffer.data(), _maxBounces);
    } else
#endif
    {
        SpectralIntegrator::RenderFrame(*_scene, cam, _frameBuffer.data(),
                                         _samples, _depthBuffer.data(), _maxBounces,
                                         _objectIdBuffer.data(), _materialIdBuffer.data());
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
