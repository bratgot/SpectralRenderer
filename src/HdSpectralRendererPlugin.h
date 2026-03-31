#pragma once

#include "HdSpectralApi.h"
#include <pxr/imaging/hd/rendererPlugin.h>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// HdSpectralRendererPlugin
//
//   This is the entry point Hydra discovers via plugInfo.json.  Its only job
//   is to construct and destroy HdSpectralRenderDelegate instances.
//
//   The TF_REGISTRY_FUNCTION macro in the .cpp registers this class with
//   HdRendererPluginRegistry under the type name "HdSpectralRendererPlugin",
//   which must match the key in plugInfo.json exactly.
// ---------------------------------------------------------------------------
class HDSPECTRAL_API HdSpectralRendererPlugin final : public HdRendererPlugin
{
public:
    HdSpectralRendererPlugin()  = default;
    ~HdSpectralRendererPlugin() override = default;

    // Non-copyable / non-movable (PXR convention for plugins)
    HdSpectralRendererPlugin(const HdSpectralRendererPlugin&)            = delete;
    HdSpectralRendererPlugin& operator=(const HdSpectralRendererPlugin&) = delete;

    // -----------------------------------------------------------------------
    // HdRendererPlugin interface
    // -----------------------------------------------------------------------

    /// Create a new render delegate, optionally pre-seeded with settings.
    HdRenderDelegate* CreateRenderDelegate() override;
    HdRenderDelegate* CreateRenderDelegate(
        HdRenderSettingsMap const& settingsMap) override;

    /// Destroy a delegate previously created by this plugin.
    void DeleteRenderDelegate(HdRenderDelegate* renderDelegate) override;

    /// Return true if this renderer can run on the current machine.
    /// Phase 1: always true (CPU path).  Phase 3: also check optixInit().
    bool IsSupported(bool gpuEnabled = true) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE
