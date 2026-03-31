#include "HdSpectralRendererPlugin.h"
#include "HdSpectralRenderDelegate.h"

#include <pxr/imaging/hd/rendererPluginRegistry.h>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Plugin registration
//
//   This macro expands to a static initialiser that registers
//   HdSpectralRendererPlugin with Hydra's plugin registry.
//   It runs when the DLL is loaded (i.e. when PXR_PLUGINPATH_NAME points
//   at the directory containing our plugInfo.json and Nuke starts).
//
//   The string name ("HdSpectralRendererPlugin") must match:
//     1. The C++ class name
//     2. The "Types" key in plugInfo.json
// ---------------------------------------------------------------------------
TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdSpectralRendererPlugin>();
}

// ---------------------------------------------------------------------------
// Factory methods
// ---------------------------------------------------------------------------

HdRenderDelegate*
HdSpectralRendererPlugin::CreateRenderDelegate()
{
    return new HdSpectralRenderDelegate();
}

HdRenderDelegate*
HdSpectralRendererPlugin::CreateRenderDelegate(
    HdRenderSettingsMap const& settingsMap)
{
    return new HdSpectralRenderDelegate(settingsMap);
}

void
HdSpectralRendererPlugin::DeleteRenderDelegate(HdRenderDelegate* renderDelegate)
{
    delete renderDelegate;
}

bool
HdSpectralRendererPlugin::IsSupported(bool /*gpuEnabled*/) const
{
    // Phase 1: CPU path is always available.
    // Phase 3: also attempt optixInit() here and cache the result.
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
