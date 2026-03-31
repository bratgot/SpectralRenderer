#include "HdSpectralRenderPass.h"
#include "HdSpectralRenderBuffer.h"
#include "SpectralIntegrator.h"

#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/diagnostic.h>

PXR_NAMESPACE_OPEN_SCOPE

HdSpectralRenderPass::HdSpectralRenderPass(
    HdRenderIndex*           index,
    HdRprimCollection const& collection,
    SpectralScene*           scene)
    : HdRenderPass(index, collection)
    , _scene(scene)
{
}

HdSpectralRenderPass::~HdSpectralRenderPass() = default;

// ---------------------------------------------------------------------------
// _Execute  — one frame, delegates all ray tracing to SpectralIntegrator
// ---------------------------------------------------------------------------
void HdSpectralRenderPass::_Execute(
    HdRenderPassStateSharedPtr const& renderPassState,
    TfTokenVector const&              /*renderTags*/)
{
    _converged = false;

    HdSpectralRenderBuffer* beauty = _GetBeautyBuffer(renderPassState);
    if (!beauty) {
        TF_WARN("HdSpectral: no beauty render buffer — nothing to render");
        _converged = true;
        return;
    }

    const unsigned int W = beauty->GetWidth();
    const unsigned int H = beauty->GetHeight();
    if (W == 0 || H == 0) { _converged = true; return; }

    // Build camera
    const HdCamera* camera = renderPassState->GetCamera();
    if (!camera) {
        TF_WARN("HdSpectral: no camera in render pass state");
        _converged = true;
        return;
    }

    SpectralCamera cam;
    cam.imageWidth  = W;
    cam.imageHeight = H;
    cam.viewToWorld = camera->GetTransform().GetInverse();
    cam.projInverse = renderPassState->GetProjectionMatrix().GetInverse();

    // Allocate a temporary frame buffer, render into it, blit to beauty AOV
    std::vector<float> frameBuf(static_cast<size_t>(W) * H * 4, 0.f);
    SpectralIntegrator::RenderFrame(*_scene, cam, frameBuf.data(), /*spp=*/1);

    // Copy into the HdRenderBuffer row by row
    void* mapped = beauty->Map();
    if (mapped) {
        std::memcpy(mapped, frameBuf.data(),
                    static_cast<size_t>(W) * H * 4 * sizeof(float));
        beauty->Unmap();
    }

    _converged = true;
}

// ---------------------------------------------------------------------------
// _GetBeautyBuffer
// ---------------------------------------------------------------------------
HdSpectralRenderBuffer*
HdSpectralRenderPass::_GetBeautyBuffer(
    const HdRenderPassStateSharedPtr& state) const
{
    for (const HdRenderPassAovBinding& binding : state->GetAovBindings()) {
        if (binding.aovName == HdAovTokens->color)
            return dynamic_cast<HdSpectralRenderBuffer*>(binding.renderBuffer);
    }
    return nullptr;
}

PXR_NAMESPACE_CLOSE_SCOPE
