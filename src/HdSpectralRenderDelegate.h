#pragma once

#include "HdSpectralApi.h"
#include "SpectralScene.h"

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderThread.h>

#include <memory>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

class HdSpectralRenderParam;

// ---------------------------------------------------------------------------
// HdSpectralRenderDelegate
//
//   Central coordinator for the Spectral renderer inside Hydra.
//
//   Responsibilities:
//     • Declare which prim types (mesh, camera, light, renderBuffer) we support
//     • Construct the rprim/sprim/bprim instances that Hydra calls Sync() on
//     • Own the SpectralScene (shared read-only with render pass)
//     • Expose render settings (samples-per-pixel, bounces, etc.) as knobs
//     • Create the HdSpectralRenderPass that executes the integrator
//
//   Lifetime: one delegate per "Spectral" renderer session in Nuke.
//   Nuke creates it when the user selects "Spectral (CPU)" in the renderer
//   dropdown and destroys it when switching away or closing the viewer.
// ---------------------------------------------------------------------------
class HDSPECTRAL_API HdSpectralRenderDelegate final : public HdRenderDelegate
{
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------
    HdSpectralRenderDelegate();
    explicit HdSpectralRenderDelegate(HdRenderSettingsMap const& settingsMap);
    ~HdSpectralRenderDelegate() override;

    // Non-copyable
    HdSpectralRenderDelegate(const HdSpectralRenderDelegate&)            = delete;
    HdSpectralRenderDelegate& operator=(const HdSpectralRenderDelegate&) = delete;

    // -----------------------------------------------------------------------
    // HdRenderDelegate — supported prim types
    // -----------------------------------------------------------------------
    const TfTokenVector& GetSupportedRprimTypes() const override;
    const TfTokenVector& GetSupportedSprimTypes() const override;
    const TfTokenVector& GetSupportedBprimTypes() const override;

    // -----------------------------------------------------------------------
    // HdRenderDelegate — resource & settings management
    // -----------------------------------------------------------------------
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;

    HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;

    // -----------------------------------------------------------------------
    // HdRenderDelegate — render pass factory
    // -----------------------------------------------------------------------
    HdRenderPassSharedPtr CreateRenderPass(
        HdRenderIndex*              index,
        HdRprimCollection const&    collection) override;

    // -----------------------------------------------------------------------
    // HdRenderDelegate — rprim factory  (geometry)
    // -----------------------------------------------------------------------
    HdRprim* CreateRprim(TfToken const& typeId,
                         SdfPath const& rprimId) override;
    void     DestroyRprim(HdRprim* rPrim) override;

    // -----------------------------------------------------------------------
    // HdRenderDelegate — sprim factory  (state: camera, lights)
    // -----------------------------------------------------------------------
    HdSprim* CreateSprim(TfToken const& typeId,
                         SdfPath const& sprimId) override;
    HdSprim* CreateFallbackSprim(TfToken const& typeId) override;
    void     DestroySprim(HdSprim* sPrim) override;

    // -----------------------------------------------------------------------
    // HdRenderDelegate — bprim factory  (buffers: renderBuffer)
    // -----------------------------------------------------------------------
    HdBprim* CreateBprim(TfToken const& typeId,
                         SdfPath const& bprimId) override;
    HdBprim* CreateFallbackBprim(TfToken const& typeId) override;
    void     DestroyBprim(HdBprim* bPrim) override;

    // -----------------------------------------------------------------------
    // HdRenderDelegate — synchronisation hook
    //
    //   Called once per frame after all Sync() calls complete.
    //   Use this to flush pending BVH rebuilds, upload to GPU, etc.
    //   Phase 1: no-op.  Phase 3: optixAccelBuild goes here.
    // -----------------------------------------------------------------------
    void CommitResources(HdChangeTracker* tracker) override;

    // -----------------------------------------------------------------------
    // Render parameters — passed to rprims during Sync so they can
    // invalidate the scene.  Currently just a pointer to our scene.
    // -----------------------------------------------------------------------
    HdRenderParam* GetRenderParam() const override;

    // -----------------------------------------------------------------------
    // Accessors for render pass
    // -----------------------------------------------------------------------
    SpectralScene* GetScene() const { return _scene.get(); }

    // -----------------------------------------------------------------------
    // Render settings tokens (used as knob names)
    // -----------------------------------------------------------------------
    static const TfToken kSettingSPP;        // samples per pixel
    static const TfToken kSettingMaxBounces; // max ray depth
    static const TfToken kSettingDevice;     // "cpu" | "gpu" | "auto"

private:
    void _Initialize(HdRenderSettingsMap const& settingsMap);

    // Shared scene state — written by mesh Sync, read by render pass
    std::unique_ptr<SpectralScene>      _scene;

    // Render param wraps the scene pointer for HdRprim::Sync callbacks
    std::unique_ptr<HdRenderParam>      _renderParam;

    // Shared resource registry (textures, etc.) — stub for Phase 1
    HdResourceRegistrySharedPtr         _resourceRegistry;

    // Cached render setting descriptors
    HdRenderSettingDescriptorList       _settingDescriptors;

    // Supported prim type lists — built once at construction
    static const TfTokenVector          _supportedRprimTypes;
    static const TfTokenVector          _supportedSprimTypes;
    static const TfTokenVector          _supportedBprimTypes;
};

PXR_NAMESPACE_CLOSE_SCOPE
