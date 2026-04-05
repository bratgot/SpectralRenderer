#pragma once
// SpectralStudioLight — studio lighting node for SpectralRender
// Three-point rig: key, fill, rim with shadow softness.
// Created by Marten Blumen

#include <DDImage/ShaderOp.h>
#include <DDImage/Knobs.h>
#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralStudioLight : public ShaderOp
{
public:
    explicit SpectralStudioLight(Node* node);

    int minimum_inputs() const override { return 0; }
    int maximum_inputs() const override { return 0; }

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;
    unsigned    node_color() const override { return 0xFFCC00FF; }

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    const char* getOutputSchema() const override { return "SpectralStudioLight"; }
    int32_t     getOutputType()   const override { return OUTPUT_TYPE_SURFACESHADER; }
    MaterialOpI* asMaterialOp()   override { return this; }

    static const char* const CLASS;
    static const Op::Description description;

    // --- Preset ---
    int    studioPreset = 0;     // 0=off, 1=portrait, 2=product, 3=dramatic, 4=softbox
    double mix = 1.0;

    // --- Key Light ---
    double keyAzimuth = 45.0;
    double keyElevation = 35.0;
    double keyIntensity = 5.0;
    float  keyColor[3] = {1.f, 0.97f, 0.92f};

    // --- Fill Light ---
    double fillRatio = 0.4;      // relative to key
    float  fillColor[3] = {0.85f, 0.9f, 1.f};

    // --- Rim Light ---
    double rimIntensity = 2.0;
    float  rimColor[3] = {1.f, 1.f, 1.f};

    // --- Shadow ---
    double shadowSoftness = 0.0;
};
