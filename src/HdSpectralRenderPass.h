#pragma once

#include "HdSpectralApi.h"
#include "SpectralScene.h"

#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/aov.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdSpectralRenderBuffer;

class HDSPECTRAL_API HdSpectralRenderPass final : public HdRenderPass
{
public:
    HdSpectralRenderPass(HdRenderIndex*           index,
                         HdRprimCollection const& collection,
                         SpectralScene*           scene);
    ~HdSpectralRenderPass() override;

protected:
    void _Execute(HdRenderPassStateSharedPtr const& renderPassState,
                  TfTokenVector const& renderTags) override;

    bool IsConverged() const override { return _converged; }

private:
    HdSpectralRenderBuffer* _GetBeautyBuffer(
        const HdRenderPassStateSharedPtr& state) const;

    SpectralScene* _scene;
    bool           _converged = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
