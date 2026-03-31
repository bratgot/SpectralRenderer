#include "HdSpectralRenderDelegate.h"
#include "HdSpectralRenderPass.h"
#include "HdSpectralMesh.h"
#include "HdSpectralRenderBuffer.h"

#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/imaging/hd/instancer.h>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Render setting tokens
// ---------------------------------------------------------------------------
const TfToken HdSpectralRenderDelegate::kSettingSPP        ("spectral:samplesPerPixel");
const TfToken HdSpectralRenderDelegate::kSettingMaxBounces ("spectral:maxBounces");
const TfToken HdSpectralRenderDelegate::kSettingDevice     ("spectral:device");

// ---------------------------------------------------------------------------
// Supported prim types
// ---------------------------------------------------------------------------
const TfTokenVector HdSpectralRenderDelegate::_supportedRprimTypes = {
    HdPrimTypeTokens->mesh,
};

const TfTokenVector HdSpectralRenderDelegate::_supportedSprimTypes = {
    HdPrimTypeTokens->camera,
    // Lights deferred to Phase 4 — HdLight is abstract in PXR 0.25.8
    // and requires a concrete subclass implementing all pure virtuals.
};

const TfTokenVector HdSpectralRenderDelegate::_supportedBprimTypes = {
    HdPrimTypeTokens->renderBuffer,
};

// ---------------------------------------------------------------------------
// Construction
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
    for (auto const& kv : settingsMap)
        _settingsMap[kv.first] = kv.second;

    if (_settingsMap.find(kSettingSPP)        == _settingsMap.end())
        _settingsMap[kSettingSPP]        = VtValue(16);
    if (_settingsMap.find(kSettingMaxBounces) == _settingsMap.end())
        _settingsMap[kSettingMaxBounces] = VtValue(4);
    if (_settingsMap.find(kSettingDevice)     == _settingsMap.end())
        _settingsMap[kSettingDevice]     = VtValue(std::string("cpu"));

    _settingDescriptors = {
        { "Samples per pixel", kSettingSPP,        VtValue(16)                 },
        { "Max bounces",       kSettingMaxBounces, VtValue(4)                  },
        { "Device",            kSettingDevice,     VtValue(std::string("cpu")) },
    };

    _scene       = std::make_unique<SpectralScene>();
    _renderParam = std::make_unique<HdRenderParam>();
    _resourceRegistry = std::make_shared<HdResourceRegistry>();

    TF_STATUS("HdSpectral: delegate initialised (Phase 1 — CPU normal shading)");
}

// ---------------------------------------------------------------------------
// Supported types
// ---------------------------------------------------------------------------
const TfTokenVector& HdSpectralRenderDelegate::GetSupportedRprimTypes() const { return _supportedRprimTypes; }
const TfTokenVector& HdSpectralRenderDelegate::GetSupportedSprimTypes() const { return _supportedSprimTypes; }
const TfTokenVector& HdSpectralRenderDelegate::GetSupportedBprimTypes() const { return _supportedBprimTypes; }

// ---------------------------------------------------------------------------
// Resources & settings
// ---------------------------------------------------------------------------
HdResourceRegistrySharedPtr
HdSpectralRenderDelegate::GetResourceRegistry() const { return _resourceRegistry; }

HdRenderSettingDescriptorList
HdSpectralRenderDelegate::GetRenderSettingDescriptors() const { return _settingDescriptors; }

HdRenderParam*
HdSpectralRenderDelegate::GetRenderParam() const { return _renderParam.get(); }

// ---------------------------------------------------------------------------
// GetDefaultAovDescriptor
//   Tells Hydra what pixel format and clear value to use for each AOV when
//   no explicit RenderVar prim is present.  We only handle "color" for now.
// ---------------------------------------------------------------------------
HdAovDescriptor
HdSpectralRenderDelegate::GetDefaultAovDescriptor(TfToken const& aovName) const
{
    if (aovName == HdAovTokens->color) {
        return HdAovDescriptor(
            HdFormatFloat32Vec4,  // RGBA float
            false,                // not multisampled
            VtValue(GfVec4f(0.0f, 0.0f, 0.0f, 1.0f))  // clear to opaque black
        );
    }
    if (aovName == HdAovTokens->depth) {
        return HdAovDescriptor(
            HdFormatFloat32,
            false,
            VtValue(1.0f)
        );
    }
    if (aovName == HdAovTokens->normal) {
        return HdAovDescriptor(
            HdFormatFloat32Vec3,
            false,
            VtValue(GfVec3f(0.0f))
        );
    }
    // Unknown AOV — return an empty descriptor (Hydra will skip it)
    return HdAovDescriptor();
}

// ---------------------------------------------------------------------------
// GetRenderStats
//   Returns per-frame diagnostics displayed in Nuke's render panel.
// ---------------------------------------------------------------------------
VtDictionary
HdSpectralRenderDelegate::GetRenderStats() const
{
    VtDictionary stats;
    stats["renderedTriangles"] = VtValue((int)_scene->TotalTriangles());
    stats["renderer"]          = VtValue(std::string("Spectral CPU Phase 1"));
    return stats;
}

// ---------------------------------------------------------------------------
// Render pass factory
// ---------------------------------------------------------------------------
HdRenderPassSharedPtr
HdSpectralRenderDelegate::CreateRenderPass(HdRenderIndex*           index,
                                            HdRprimCollection const& collection)
{
    return std::make_shared<HdSpectralRenderPass>(index, collection, _scene.get());
}

// ---------------------------------------------------------------------------
// Rprim factory
// ---------------------------------------------------------------------------
HdRprim*
HdSpectralRenderDelegate::CreateRprim(TfToken const& typeId, SdfPath const& rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh)
        return new HdSpectralMesh(rprimId, _scene.get());
    TF_CODING_ERROR("HdSpectral: unsupported rprim '%s'", typeId.GetText());
    return nullptr;
}

void HdSpectralRenderDelegate::DestroyRprim(HdRprim* rPrim) { delete rPrim; }

// ---------------------------------------------------------------------------
// Sprim factory — camera only for Phase 1
// ---------------------------------------------------------------------------
HdSprim*
HdSpectralRenderDelegate::CreateSprim(TfToken const& typeId, SdfPath const& sprimId)
{
    if (typeId == HdPrimTypeTokens->camera)
        return new HdCamera(sprimId);
    // All light types deferred to Phase 4
    return nullptr;
}

HdSprim*
HdSpectralRenderDelegate::CreateFallbackSprim(TfToken const& typeId)
{
    if (typeId == HdPrimTypeTokens->camera)
        return new HdCamera(SdfPath::EmptyPath());
    return nullptr;
}

void HdSpectralRenderDelegate::DestroySprim(HdSprim* sPrim) { delete sPrim; }

// ---------------------------------------------------------------------------
// Bprim factory
// ---------------------------------------------------------------------------
HdBprim*
HdSpectralRenderDelegate::CreateBprim(TfToken const& typeId, SdfPath const& bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer)
        return new HdSpectralRenderBuffer(bprimId);
    TF_CODING_ERROR("HdSpectral: unsupported bprim '%s'", typeId.GetText());
    return nullptr;
}

HdBprim*
HdSpectralRenderDelegate::CreateFallbackBprim(TfToken const& typeId)
{
    return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdSpectralRenderDelegate::DestroyBprim(HdBprim* bPrim) { delete bPrim; }

// ---------------------------------------------------------------------------
// CommitResources — Phase 1 no-op
// ---------------------------------------------------------------------------
void HdSpectralRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/) {}

// ---------------------------------------------------------------------------
// Instancer factory — pure virtual in PXR 0.25.8, deferred to Phase 2
// ---------------------------------------------------------------------------
HdInstancer*
HdSpectralRenderDelegate::CreateInstancer(HdSceneDelegate* /*delegate*/,
                                           SdfPath const&   /*id*/)
{
    // Point instancing (USD PointInstancer prim) not yet supported.
    // Returning nullptr is valid — Hydra skips instanced prims gracefully.
    return nullptr;
}

void
HdSpectralRenderDelegate::DestroyInstancer(HdInstancer* instancer)
{
    delete instancer;
}

PXR_NAMESPACE_CLOSE_SCOPE