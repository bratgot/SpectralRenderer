#pragma once
// SpectralVolumeMaterial — volume shader node for SpectralRender
// ShaderOp-based (same node shape as SpectralSurface).
// Created by Marten Blumen

#include <DDImage/ShaderOp.h>
#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>

#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralVolumeMaterial : public ShaderOp
{
public:
    explicit SpectralVolumeMaterial(Node* node);

    int minimum_inputs() const override { return 0; }
    int maximum_inputs() const override { return 1; }
    const char* input_label(int idx, char*) const override {
        return (idx == 0) ? "tex" : "";
    }
    bool test_input(int idx, Op* op) const override;

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    // MaterialOpI interface
    const char* getOutputSchema() const override { return "SpectralVolumeMaterial"; }
    int32_t     getOutputType()   const override { return OUTPUT_TYPE_SURFACESHADER; }
    MaterialOpI* asMaterialOp()   override { return this; }

    static const char* const CLASS;
    static const Op::Description description;

    // --- Volume shading parameters (read by SpectralRender) ---
    double extinction    = 5.0;
    double scattering    = 3.0;
    double densityMult   = 1.0;
    double stepSize      = 0.5;
    int    maxSteps      = 256;

    double gForward      = 0.65;
    double gBackward     = -0.25;
    double lobeMix       = 0.70;
    double powder        = 2.0;
    bool   jitter        = true;

    double emissionIntensity = 2.0;
    double tempMin       = 500.0;
    double tempMax       = 6500.0;
    double flameIntensity = 5.0;
    bool   useBlackbody  = true;

    bool   chromaticExtinction = false;
    double sigmaR        = 1.0;
    double sigmaG        = 1.0;
    double sigmaB        = 1.2;

    float  scatterColor[3] = {1.f, 1.f, 1.f};
    int    preset        = 0;

    static void BlackbodyToRGB(float tempK, float& r, float& g, float& b);
};
