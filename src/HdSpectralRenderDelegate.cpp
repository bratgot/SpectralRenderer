#include "HdSpectralRenderDelegate.h"
#include "HdSpectralRenderPass.h"
#include "HdSpectralMesh.h"
#include "HdSpectralRenderBuffer.h"

#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/glf/simpleLight.h>

#include <pxr/base/tf/staticTokens.h>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Render setting token definitions
// ---------------------------------------------------------------------------
const TfToken HdSpectralRenderDelegate::kSettingSPP        ("spectral:samplesPerPixel");
const TfToken HdSpectralRenderDelegate::kSettingMaxBounces ("spectral:maxBounces");
const TfToken HdSpectralRenderDelegate::kSettingDevice     ("spectral:device");

// ---------------------------------------------------------------------------
// Supported prim type lists
//
//   Phase 1:  mesh rprims, camera/simpleLight sprims, renderBuffer bprims.
//   Phase 2+: add more light types (rectLight, diskLight, domeLight, etc.)
// ---------------------------------------------------------------------------
const TfTokenVector HdSpectralRenderDelegate::_supportedRprimTypes = {
    HdPrimTypeTokens->mesh,
};

const TfTokenVector HdSpectralRenderDelegate::_supportedSprimTypes = {
    HdPrimTypeTokens->camera,
    HdPrimTypeTokens->simpleLight,
    HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->cylinderLight,
    HdPrimTypeTokens->sphereLight,
    HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->domeLight,
};

const TfTokenVector HdSpectralRenderDelegate::_supportedBprimTypes = {
    HdPrimTypeTokens->renderBuffer,
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

HdSpectralRenderDelegate::HdSpectralRenderDelegate()
{
    _Initialize({});
}

HdSpectralRenderDelegate::HdSpectralRenderDelegate(
    HdRenderSettingsMap const& settingsMap)
{
    _Initialize(settingsMap);
}

HdSpectralRenderDelegate::~HdSpectralRenderDelegate() = default;

void
HdSpectralRenderDelegate::_Initialize(HdRenderSettingsMap const& settingsMap)
{
    // Apply any settings passed from the host (Nuke render panel)
    for (auto const& kv : settingsMap) {
        _settingsMap[kv.first] = kv.second;
    }

    // Set defaults for any unspecified settings
    if (_settingsMap.find(kSettingSPP)        == _settingsMap.end())
        _settingsMap[kSettingSPP]        = VtValue(16);
    if (_settingsMap.find(kSettingMaxBounces) == _settingsMap.end())
        _settingsMap[kSettingMaxBounces] = VtValue(4);
    if (_settingsMap.find(kSettingDevice)     == _settingsMap.end())
        _settingsMap[kSettingDevice]     = VtValue(std::string("cpu"));

    // Build setting descriptors (shown as knobs in Nuke's render panel)
    _settingDescriptors = {
        { "Samples per pixel",  kSettingSPP,        VtValue(16)                    },
        { "Max bounces",        kSettingMaxBounces, VtValue(4)                     },
        { "Device",             kSettingDevice,     VtValue(std::string("cpu"))    },
    };

    // Allocate the shared scene and render param
    _scene       = std::make_unique<SpectralScene>();
    _renderParam = std::make_unique<HdRenderParam>();   // plain base class for Phase 1

    // Stub resource registry (textures etc. added in later phases)
    _resourceRegistry = std::make_shared<HdResourceRegistry>();

    TF_STATUS("HdSpectral: delegate initialised (Phase 1 — CPU normal-shading)");
}

// ---------------------------------------------------------------------------
// HdRenderDelegate interface — supported types
// ---------------------------------------------------------------------------

const TfTokenVector&
HdSpectralRenderDelegate::GetSupportedRprimTypes() const
{
    return _supportedRprimTypes;
}

const TfTokenVector&
HdSpectralRenderDelegate::GetSupportedSprimTypes() const
{
    return _supportedSprimTypes;
}

const TfTokenVector&
HdSpectralRenderDelegate::GetSupportedBprimTypes() const
{
    return _supportedBprimTypes;
}

// ---------------------------------------------------------------------------
// Resources & settings
// ---------------------------------------------------------------------------

HdResourceRegistrySharedPtr
HdSpectralRenderDelegate::GetResourceRegistry() const
{
    return _resourceRegistry;
}

HdRenderSettingDescriptorList
HdSpectralRenderDelegate::GetRenderSettingDescriptors() const
{
    return _settingDescriptors;
}

HdRenderParam*
HdSpectralRenderDelegate::GetRenderParam() const
{
    return _renderParam.get();
}

// ---------------------------------------------------------------------------
// Render pass factory
// ---------------------------------------------------------------------------

HdRenderPassSharedPtr
HdSpectralRenderDelegate::CreateRenderPass(
    HdRenderIndex*           index,
    HdRprimCollection const& collection)
{
    return std::make_shared<HdSpectralRenderPass>(index, collection, _scene.get());
}

// ---------------------------------------------------------------------------
// Rprim factory — geometry
// ---------------------------------------------------------------------------

HdRprim*
HdSpectralRenderDelegate::CreateRprim(TfToken const& typeId,
                                      SdfPath const& rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh) {
        return new HdSpectralMesh(rprimId, _scene.get());
    }
    TF_CODING_ERROR("HdSpectral: unsupported rprim type '%s'", typeId.GetText());
    return nullptr;
}

void
HdSpectralRenderDelegate::DestroyRprim(HdRprim* rPrim)
{
    delete rPrim;
}

// ---------------------------------------------------------------------------
// Sprim factory — state objects (camera, lights)
//
//   For Phase 1 we use Hydra's built-in HdCamera and HdLight stubs.
//   These Sync themselves and expose accessor methods our render pass uses.
// ---------------------------------------------------------------------------

HdSprim*
HdSpectralRenderDelegate::CreateSprim(TfToken const& typeId,
                                      SdfPath const& sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdCamera(sprimId);
    }
    // For all light types return a simple light stub
    if (typeId == HdPrimTypeTokens->simpleLight  ||
        typeId == HdPrimTypeTokens->rectLight     ||
        typeId == HdPrimTypeTokens->diskLight     ||
        typeId == HdPrimTypeTokens->cylinderLight ||
        typeId == HdPrimTypeTokens->sphereLight   ||
        typeId == HdPrimTypeTokens->distantLight  ||
        typeId == HdPrimTypeTokens->domeLight)
    {
        return new HdLight(sprimId, typeId);
    }
    TF_CODING_ERROR("HdSpectral: unsupported sprim type '%s'", typeId.GetText());
    return nullptr;
}

HdSprim*
HdSpectralRenderDelegate::CreateFallbackSprim(TfToken const& typeId)
{
    // Fallback sprims are created for prim paths that exist in the scene
    // index but haven't been populated yet.  Same factory as normal.
    return CreateSprim(typeId, SdfPath::EmptyPath());
}

void
HdSpectralRenderDelegate::DestroySprim(HdSprim* sPrim)
{
    delete sPrim;
}

// ---------------------------------------------------------------------------
// Bprim factory — buffers (render targets / AOVs)
// ---------------------------------------------------------------------------

HdBprim*
HdSpectralRenderDelegate::CreateBprim(TfToken const& typeId,
                                      SdfPath const& bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new HdSpectralRenderBuffer(bprimId);
    }
    TF_CODING_ERROR("HdSpectral: unsupported bprim type '%s'", typeId.GetText());
    return nullptr;
}

HdBprim*
HdSpectralRenderDelegate::CreateFallbackBprim(TfToken const& typeId)
{
    return CreateBprim(typeId, SdfPath::EmptyPath());
}

void
HdSpectralRenderDelegate::DestroyBprim(HdBprim* bPrim)
{
    delete bPrim;
}

// ---------------------------------------------------------------------------
// CommitResources
//
//   Called by HdEngine::Execute() after all Sync() calls for the frame.
//   Phase 1: nothing to do — the triangle soup in SpectralScene is ready.
//   Phase 3: kick off optixAccelBuild() on the committed geometry here.
// ---------------------------------------------------------------------------

void
HdSpectralRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/)
{
    // Phase 1 no-op.
    // TF_DEBUG(HD_SPECTRAL).Msg("CommitResources: %zu triangles in scene\n",
    //     _scene->TotalTriangles());
}

PXR_NAMESPACE_CLOSE_SCOPE
