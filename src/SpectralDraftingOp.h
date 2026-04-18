#pragma once

#include <DDImage/ShaderOp.h>
#include <DDImage/Knobs.h>
#include <unordered_map>
#include <string>

#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralDraftingOp : public ShaderOp
{
public:
    explicit SpectralDraftingOp(Node* node);
    ~SpectralDraftingOp() override;

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

    float _wireColor[3]    = {1.f, 1.f, 1.f};
    float _wireThickness   = 1.0f;
    float _wireOpacity     = 1.0f;
    bool  _wireDashed      = false;
    float _wireDashLength  = 8.0f;
    float _wireGapLength   = 4.0f;
    int   _wireNth         = 1;
    int   _wireStyle       = 0;
    float _gridDensity     = 10.0f;
    bool  _showTriangles   = false;
    // Architectural
    float _archSilhouetteWeight = 3.0f;
    float _archMediumWeight = 1.5f;
    float _archThinWeight = 0.6f;
    float _archSilhouetteColor[3] = {0.f, 0.f, 0.f};
    float _archThinOpacity = 0.4f;
    // Pencil
    float _pencilWobble = 0.3f;
    float _pencilPressure = 0.6f;
    bool  _pencilCrossHatch = true;
    float _pencilHatchDensity = 4.0f;
    float _pencilHatchAngle = 45.0f;
    // Topo
    int   _topoDirection = 0;
    float _topoUpVector[3] = {0.f, 1.f, 0.f};
    float _topoContourInterval = 0.5f;
    int   _topoMajorEvery = 5;
    // Antialiasing
    int   _aaMode = 1;       // 0=off, 1=edge, 2=2x2, 3=4x4
    float _aaWidth = 1.5f;   // smoothstep band in pixels (mode >= 1)

    void _SetShaderProperties(usg::ShaderDesc& desc, const MaterialContext& rtx);

public:
    struct DraftingParams {
        float  thickness     = 1.f;
        float  opacity       = 1.f;
        float  color[3]      = {1.f, 1.f, 1.f};
        bool   dashed        = false;
        float  dashLength    = 8.f;
        float  gapLength     = 4.f;
        int    nth           = 1;
        int    style         = 0;
        float  gridDensity   = 10.f;
        bool   showTriangles = false;
        // Architectural
        float  archSilhouetteWeight = 3.f;
        float  archMediumWeight = 1.5f;
        float  archThinWeight = 0.6f;
        float  archSilhouetteColor[3] = {0.f, 0.f, 0.f};
        float  archThinOpacity = 0.4f;
        // Pencil
        float  pencilWobble = 0.3f;
        float  pencilPressure = 0.6f;
        bool   pencilCrossHatch = true;
        float  pencilHatchDensity = 4.f;
        float  pencilHatchAngle = 45.f;
        // Topo
        int    topoDirection = 0;
        float  topoUpVector[3] = {0.f, 1.f, 0.f};
        float  topoContourInterval = 0.5f;
        int    topoMajorEvery = 5;
        // Antialiasing
        int    aaMode = 1;
        float  aaWidth = 1.5f;
    };
    static std::unordered_map<std::string, DraftingParams>& GetRegistry();
    void RegisterParams();
};
