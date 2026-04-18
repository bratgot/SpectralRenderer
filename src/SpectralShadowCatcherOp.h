#pragma once

#include <DDImage/ShaderOp.h>
#include <DDImage/Knobs.h>
#include <unordered_map>
#include <string>

#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralShadowCatcherOp : public ShaderOp
{
public:
    explicit SpectralShadowCatcherOp(Node* node);

    int minimum_inputs() const override { return 0; }
    int maximum_inputs() const override { return 0; }

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    const char* getOutputSchema() const override { return "UsdPreviewSurface"; }
    int32_t     getOutputType()   const override { return OUTPUT_TYPE_SURFACESHADER; }
    MaterialOpI* asMaterialOp()   override { return this; }

    usg::ShaderDesc* createShaderGraph(int32_t outputType,
        const MaterialContext& rtx, usg::ShaderDescGroup& shaderGroup) override;
    void updateShaderGraphOverrides(int32_t outputType,
        const MaterialContext& rtx, usg::ShaderDescGroup& shaderGroup) override;

    static const Op::Description description;

private:
    static const char* const CLASS;

    float _shadowIntensity  = 1.0f;
    float _shadowColor[3]   = {0.f, 0.f, 0.f};
    bool  _selfShadow       = false;

    void _SetShaderProperties(usg::ShaderDesc& desc, const MaterialContext& rtx);

public:
    struct ShadowCatcherParams {
        float  shadowIntensity = 1.f;
        float  shadowColor[3]  = {0.f, 0.f, 0.f};
        bool   selfShadow      = false;
    };
    static std::unordered_map<std::string, ShadowCatcherParams>& GetRegistry();
    void RegisterParams();
};
