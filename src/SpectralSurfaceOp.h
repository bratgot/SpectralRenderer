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

#include <unordered_map>
#include <string>

#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralSurfaceOp : public ShaderOp
{
public:
    explicit SpectralSurfaceOp(Node* node);
    ~SpectralSurfaceOp() override;

    int minimum_inputs() const override { return 2; }
    int maximum_inputs() const override { return 2; }
    const char* input_label(int idx, char*) const override {
        switch(idx) {
            case 0: return "tex";
            case 1: return "disp/bump";
            default: return "";
        }
    }
    bool test_input(int idx, Op* op) const override;

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
    int   _lastAppliedPreset = 0;  // tracker -- see notes in knob_changed
    float _abbeNumber       = 0.0f;   // dispersion (0=no dispersion, ~60=crown glass, ~30=flint glass)
    float _thinFilmThickness = 0.0f;  // nm (0=disabled, 200-800nm for interference)
    float _displacementScale = 0.0f;    // world units (0=disabled)
    float _displacementMidpoint = 0.0f; // 0.5=centered, 0=outward only
    int   _metalType = 0;                // 0=none, 1=gold, 2=copper, 3=silver, 4=alu, 5=iron, 6=ti
    float _textureBlend = 1.0f;          // 0=base color only, 1=full texture
    float _absorptionColor[3] = {1.f, 1.f, 1.f}; // volume color (white=clear)
    float _absorptionDensity = 0.f;       // 0=clear, higher=darker
    int   _mapMode = 0;                      // 0=bump, 1=displacement
    int   _dispType = 0;                     // 0=scalar, 1=vector tangent, 2=vector object
    float _bumpStrength = 1.0f;              // bump map intensity
    float _gratingSpacing = 0.f;
    float _gratingStrength = 1.f;
    float _fluorAbsorb = 0.f;
    float _fluorEmit = 0.f;
    float _fluorStrength = 0.f;
    float _sssColor[3] = {0.f, 0.f, 0.f};
    float _sssRadius = 0.f;

    // 2026-04-20: unified back to a single master preset dropdown
    // mirroring SpectralVolumeMaterial. The earlier 8-category split
    // had re-entrant callback issues that caused some presets (jade,
    // spectral category) to fail after a few selections.
    const char* _displacementFile = "";  // displacement map path

    void _ApplyPreset(int preset);
    void _SetShaderProperties(usg::ShaderDesc& desc, const MaterialContext& rtx);

public:
    // Spectral parameter registry — shared between SpectralSurfaceOp and the renderer.
    // Key = Nuke node name (e.g. "SpectralSurface1"), accessed by the material reader.
    struct SpectralParams {
        float abbeNumber       = 0.f;
        float thinFilmThickness = 0.f;
        float displacementScale = 0.f;
        float displacementMidpoint = 0.0f;
        std::string displacementFile;
        int metalType = 0;
        float textureBlend = 1.0f;
        float absorptionColor[3] = {1.f, 1.f, 1.f};
        float absorptionDensity = 0.f;
        int mapMode = 0;
        int dispType = 0;
        float bumpStrength = 1.0f;
        float gratingSpacing = 0.f;
        float gratingStrength = 1.f;
        float fluorAbsorb = 0.f;
        float fluorEmit = 0.f;
        float fluorStrength = 0.f;
        float sssColor[3] = {0.f, 0.f, 0.f};
        float sssRadius = 0.f;
        Op* dispIop = nullptr;
        Op* texIop  = nullptr;   // base color texture Iop
        Op* mapIop  = nullptr;   // bump or displacement map Iop
    };
    static std::unordered_map<std::string, SpectralParams>& GetRegistry();
    void RegisterParams();
};
