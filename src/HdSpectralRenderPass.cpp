#include "HdSpectralRenderPass.h"
#include "HdSpectralRenderBuffer.h"
#include "SpectralIntegrator.h"

#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/diagnostic.h>

#include <cstring>
#include <string>
#include <vector>

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

    // The integrator can fill a full AOV set in a single trace (see
    // SpectralIntegrator::AOVBuffers). Point each requested AOV at temp storage
    // sized to its channel count, render once, then blit each into its buffer.
    const size_t px = static_cast<size_t>(W) * H;
    std::vector<float> beautyBuf(px * 4, 0.f);

    SpectralIntegrator::AOVBuffers aov{};
    std::vector<float> depthT, objIdT, matIdT, aoT;
    std::vector<std::vector<float>> hold;   // storage for the vec2/vec3 AOVs
    hold.reserve(24);                       // reserve so data() pointers stay valid
    auto alloc = [&](int ch) -> float* { hold.emplace_back(px * ch, 0.f); return hold.back().data(); };

    struct AovOut { HdSpectralRenderBuffer* buf; const float* src; int ch; };
    std::vector<AovOut> outs;

    for (const HdRenderPassAovBinding& b : renderPassState->GetAovBindings()) {
        auto* rb = dynamic_cast<HdSpectralRenderBuffer*>(b.renderBuffer);
        if (!rb) continue;
        const TfToken& t = b.aovName;
        const std::string n = t.GetString();
        if      (t == HdAovTokens->color)  outs.push_back({rb, beautyBuf.data(), 4});
        else if (t == HdAovTokens->depth || t == HdAovTokens->cameraDepth) { depthT.assign(px, 0.f); outs.push_back({rb, depthT.data(), 1}); }
        else if (t == HdAovTokens->primId) { objIdT.assign(px, 0.f); outs.push_back({rb, objIdT.data(), 1}); }
        else if (n == "materialId")        { matIdT.assign(px, 0.f); outs.push_back({rb, matIdT.data(), 1}); }
        else if (n == "ao")                { aoT.assign(px, 0.f);    outs.push_back({rb, aoT.data(),    1}); }
        else if (t == HdAovTokens->normal || t == HdAovTokens->Neye || n == "normal" || n == "N") { aov.normal = alloc(3); outs.push_back({rb, aov.normal, 3}); }
        else if (n == "position" || n == "P" || n == "Peye") { aov.position = alloc(3); outs.push_back({rb, aov.position, 3}); }
        else if (n == "pRef")              { aov.pRef = alloc(3);              outs.push_back({rb, aov.pRef, 3}); }
        else if (n == "albedo")            { aov.albedo = alloc(3);           outs.push_back({rb, aov.albedo, 3}); }
        else if (n == "direct")            { aov.direct = alloc(3);           outs.push_back({rb, aov.direct, 3}); }
        else if (n == "indirect")          { aov.indirect = alloc(3);         outs.push_back({rb, aov.indirect, 3}); }
        else if (n == "emission")          { aov.emission = alloc(3);         outs.push_back({rb, aov.emission, 3}); }
        else if (n == "diffuseDirect")     { aov.diffuseDirect = alloc(3);    outs.push_back({rb, aov.diffuseDirect, 3}); }
        else if (n == "specularDirect")    { aov.specularDirect = alloc(3);   outs.push_back({rb, aov.specularDirect, 3}); }
        else if (n == "diffuseIndirect")   { aov.diffuseIndirect = alloc(3);  outs.push_back({rb, aov.diffuseIndirect, 3}); }
        else if (n == "specularIndirect")  { aov.specularIndirect = alloc(3); outs.push_back({rb, aov.specularIndirect, 3}); }
        else if (n == "transmission")      { aov.transmission = alloc(3);     outs.push_back({rb, aov.transmission, 3}); }
        else if (n == "uv" || n == "st")   { aov.uv = alloc(2);               outs.push_back({rb, aov.uv, 2}); }
    }
    if (outs.empty()) outs.push_back({beauty, beautyBuf.data(), 4});   // color always

    SpectralIntegrator::RenderFrame(
        *_scene, cam, beautyBuf.data(), /*spp=*/1,
        depthT.empty() ? nullptr : depthT.data(), /*maxBounces=*/4,
        objIdT.empty() ? nullptr : objIdT.data(),
        matIdT.empty() ? nullptr : matIdT.data(),
        &aov,
        aoT.empty() ? nullptr : aoT.data());

    for (const AovOut& o : outs) {
        if (void* mapped = o.buf->Map()) {
            std::memcpy(mapped, o.src, px * o.ch * sizeof(float));
            o.buf->Unmap();
        }
        o.buf->SetConverged(true);
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
