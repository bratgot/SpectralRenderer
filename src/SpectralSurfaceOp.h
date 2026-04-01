#pragma once

// ---------------------------------------------------------------------------
// SpectralSurfaceOp
//
//   Custom Nuke 17 ShaderOp for spectral rendering.
//   Emits a UsdPreviewSurface with additional spectral properties:
//     - Dispersion (Abbe number for glass/prism effects)
//     - Thin-film interference thickness
//     - Spectral preset (glass, diamond, copper, gold, etc.)
//
//   Connects via GeoBindMaterial like BasicSurface.
// ---------------------------------------------------------------------------

#include <DDImage/ShaderOp.h>
#include <DDImage/Knobs.h>

#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralSurfaceOp : public ShaderOp
{
public:
    explicit SpectralSurfaceOp(Node* node);

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    // MaterialOpI interface
    const char* getOutputSchema() const override { return "UsdPreviewSurface"; }
    int32_t     getOutputType()   const override { return OUTPUT_TYPE_SURFACESHADER; }
    MaterialOpI* asMaterialOp()   override { return this; }

    // ShaderDesc creation
    usg::ShaderDesc* createShaderGraph(int32_t                outputType,
                                       const MaterialContext& rtx,
                                       usg::ShaderDescGroup&  shaderGroup) override;

    void updateShaderGraphOverrides(int32_t                outputType,
                                    const MaterialContext& rtx,
                                    usg::ShaderDescGroup&  shaderGroup) override;

    static const Op::Description description;

private:
    static const char* const CLASS;

    // Standard PBR knobs
    float _diffuseColor[3]  = { 0.8f, 0.8f, 0.8f };
    float _metallic         = 0.0f;
    float _roughness        = 0.5f;
    float _ior              = 1.5f;
    float _opacity          = 1.0f;
    float _emissiveColor[3] = { 0.0f, 0.0f, 0.0f };
    float _clearcoat        = 0.0f;
    float _clearcoatRoughness = 0.0f;

    // Spectral-specific knobs
    int   _spectralPreset   = 0;   // 0=custom, 1=glass, 2=diamond, 3=copper, 4=gold, 5=silver, 6=aluminium
    float _abbeNumber       = 0.0f;   // dispersion (0=no dispersion, ~60=crown glass, ~30=flint glass)
    float _thinFilmThickness = 0.0f;  // nm (0=disabled, 200-800nm for interference)

    void _ApplyPreset(int preset);
    void _SetShaderProperties(usg::ShaderDesc& desc, const MaterialContext& rtx);
};
