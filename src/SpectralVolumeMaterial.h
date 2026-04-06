#pragma once
// SpectralVolumeMaterial — volume shader node for SpectralRender
// All look-dev controls: presets, density, phase, emission, noise.
// Grid selection stays on SpectralVDBRead (it discovers them from the file).
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
    const char* input_label(int idx, char*) const override { return (idx==0)?"tex":""; }
    bool test_input(int idx, Op* op) const override;

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;
    unsigned    node_color() const override { return 0xFF6B35FF; }

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    const char* getOutputSchema() const override { return "SpectralVolumeMaterial"; }
    int32_t     getOutputType()   const override { return OUTPUT_TYPE_SURFACESHADER; }
    MaterialOpI* asMaterialOp()   override { return this; }

    static const char* const CLASS;
    static const Op::Description description;

    // --- Preset ---
    int    preset = 0;

    // --- Density ---
    double extinction    = 2.0;
    double scattering    = 1.5;
    double densityMult   = 1.0;
    float  scatterColor[3] = {1.f, 1.f, 1.f};

    // --- Phase Function ---
    int    phaseMode     = 0;       // 0=Dual-lobe HG, 1=Approximate Mie
    double mieDropletD   = 2.0;
    double gForward      = 0.65;
    double gBackward     = -0.25;
    double lobeMix       = 0.70;
    double powder        = 2.0;
    double gradientMix   = 0.0;
    bool   jitter        = true;

    // --- Emission ---
    double emissionIntensity = 2.0;
    double tempMin       = 500.0;
    double tempMax       = 6500.0;
    double flameIntensity = 5.0;
    bool   useBlackbody  = true;

    // --- Chromatic Extinction ---
    bool   chromaticExtinction = false;
    double sigmaR = 1.0, sigmaG = 1.0, sigmaB = 1.2;

    // --- Noise ---
    bool   noiseEnable    = false;
    bool   noiseNormalize = true;
    double noiseScale     = 4.0;
    double noiseStrength  = 0.3;
    int    noiseOctaves   = 3;
    double noiseRoughness = 0.5;
    int    noiseLodPreset = 0;  // 0=Custom,1=Subtle,2=Natural,3=Detailed,4=Extreme,5=Wispy

    // --- Multiple Scattering ---
    bool   msApprox = true;
    float  msTint[3] = {1.f, 0.97f, 0.95f};

    static void BlackbodyToRGB(float tempK, float& r, float& g, float& b);
};
