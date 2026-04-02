#include "HdSpectralRendererPlugin.h"
#include "HdSpectralRenderDelegate.h"

#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/base/tf/type.h>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Plugin registration
//
//   HdRendererPluginRegistry::Define registers both the TfType and the
//   factory in one call.  The type name must match plugInfo.json "Types" key.
//
//   Note: PXR may emit a "missing TfType registration" warning during
//   startup if it parses plugInfo.json before this DLL's static
//   initialisers run.  The warning is resolved once the DLL loads
//   and TF_REGISTRY_FUNCTION executes — "Spectral (CPU)" still appears
//   in the renderer dropdown.
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
