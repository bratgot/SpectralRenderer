#pragma once
// SpectralEnvLight — environment/sky lighting node for SpectralRender
// Sky presets, solar position, HDRI environment maps.
// Created by Marten Blumen

#include <DDImage/ShaderOp.h>
#include <DDImage/Knobs.h>
#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralEnvLight : public ShaderOp
{
public:
    explicit SpectralEnvLight(Node* node);

    int minimum_inputs() const override { return 0; }
    int maximum_inputs() const override { return 0; }

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;
    unsigned    node_color() const override { return 0xFFCC00FF; } // warm yellow

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    const char* getOutputSchema() const override { return "SpectralEnvLight"; }
    int32_t     getOutputType()   const override { return OUTPUT_TYPE_SURFACESHADER; }
    MaterialOpI* asMaterialOp()   override { return this; }

    static const char* const CLASS;
    static const Op::Description description;

    // --- Sky Model ---
    int    skyPreset = 1;        // 0=off, 1+=preset
    double sunElevation = 45.0;
    double sunAzimuth = 180.0;
    double sunIntensity = 5.0;   // squared for exponential
    double skyIntensity = 1.0;   // squared for exponential

    // --- Solar Position ---
    int    locationPreset = 0;
    double latitude = 51.5;
    double longitude = -0.12;
    double timeOfDay = 12.0;
    int    dayOfYear = 172;

    // --- HDRI ---
    const char* hdriFile = "";
    double hdriIntensity = 1.0;
    double hdriRotate = 0.0;

    // --- Environment Controls ---
    double envIntensity = 1.0;
    double envRotate = 0.0;
    double envDiffuse = 0.5;
    int    envMode = 1;          // 0=average, 1=SH+virtual
    int    envVirtualLights = 2;
    bool   useReSTIR = false;

    // Computed sun direction (from solar position or manual)
    void ComputeSunPosition();
};
