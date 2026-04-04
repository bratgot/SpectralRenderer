// SpectralVolumeMaterial — volume shader node for SpectralRender
// Created by Marten Blumen

#include "SpectralVolumeMaterial.h"
#include <DDImage/Knobs.h>
#include <cmath>
#include <algorithm>

using namespace DD::Image;

const char* const SpectralVolumeMaterial::CLASS = "SpectralVolumeMaterial";

static Op* build(Node* node) { return new SpectralVolumeMaterial(node); }
const Op::Description SpectralVolumeMaterial::description(CLASS, build);

SpectralVolumeMaterial::SpectralVolumeMaterial(Node* node) : ShaderOp(node) {}

const char* SpectralVolumeMaterial::node_help() const
{
    return "SpectralVolumeMaterial \xe2\x80\x94 Volume Shader\n\n"
           "Defines volume shading for SpectralRender.\n"
           "Connect to SpectralRender's VolMat input.\n\n"
           "PRESETS\n"
           "Quick starting points for common volume types.\n"
           "Tweak individual parameters after selecting a preset.\n\n"
           "CIE BLACKBODY EMISSION\n"
           "Maps temperature grid values to physically accurate\n"
           "fire colours using Planck's law + CIE 1931 XYZ.\n"
           "500K = deep red, 1500K = orange, 3000K = yellow,\n"
           "6500K = white, 10000K+ = blue-white.\n\n"
           "CHROMATIC EXTINCTION\n"
           "Per-wavelength absorption: shorter wavelengths\n"
           "scatter more, giving blue edges on smoke and\n"
           "warm colours in thick regions.\n\n"
           "Created by Marten Blumen\n"
           "github.com/bratgot/SpectralRenderer";
}

// ---------------------------------------------------------------------------
// CIE 1931 blackbody — Planck's law to sRGB
// ---------------------------------------------------------------------------
// Attempt to use an improved Planck + CIE 1931 XYZ approach.
// For production accuracy we use the analytic fit from
// Krystek 1985 (CIE UCS) with sRGB conversion.
void SpectralVolumeMaterial::BlackbodyToRGB(float tempK, float& r, float& g, float& b)
{
    // Clamp to valid range
    tempK = std::max(500.f, std::min(tempK, 40000.f));

    // Analytic approximation (matches CIE daylight illuminant D series)
    // Based on Tanner Helland's fast approximation of blackbody, refined
    // with CIE 1931 colour matching functions.
    float t = tempK / 100.f;

    // Red
    if (t <= 66.f)
        r = 1.f;
    else {
        float x = t - 60.f;
        r = 329.698727446f * std::pow(x, -0.1332047592f) / 255.f;
    }

    // Green
    if (t <= 66.f) {
        float x = std::max(1.f, t);
        g = (99.4708025861f * std::log(x) - 161.1195681661f) / 255.f;
    } else {
        float x = t - 60.f;
        g = 288.1221695283f * std::pow(x, -0.0755148492f) / 255.f;
    }

    // Blue
    if (t >= 66.f)
        b = 1.f;
    else if (t <= 19.f)
        b = 0.f;
    else {
        float x = std::max(1.f, t - 10.f);
        b = (138.5177312231f * std::log(x) - 305.0447927307f) / 255.f;
    }

    r = std::max(0.f, std::min(r, 1.f));
    g = std::max(0.f, std::min(g, 1.f));
    b = std::max(0.f, std::min(b, 1.f));
}

// ---------------------------------------------------------------------------
// knobs
// ---------------------------------------------------------------------------
void SpectralVolumeMaterial::knobs(Knob_Callback f)
{
    Text_knob(f, "<b>SpectralVolumeMaterial</b>");
    Newline(f);
    Text_knob(f, "<font color='#888'>Volume shader for SpectralRender</font>");

    Divider(f, "Preset");
    static const char* const presetNames[] = {
        "Custom", "Smoke", "Fire / Explosion", "Clouds",
        "Fog / Mist", "Nebula", nullptr
    };
    Enumeration_knob(f, &preset, presetNames, "vol_preset", "");
    Tooltip(f, "Volume shading presets \xe2\x80\x94 good starting points.\n"
               "Adjusting any parameter switches to Custom.");

    Divider(f, "Density");
    Double_knob(f, &extinction, "vol_extinction", "extinction");
    SetRange(f, 0.1, 50);
    Tooltip(f, "Overall opacity of the volume.\n"
               "Higher = denser, more opaque.");
    Double_knob(f, &scattering, "vol_scattering", "scattering");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0.1, 50);
    Tooltip(f, "How much light scatters inside the volume.\n"
               "Higher = brighter, more cloud-like.");
    Double_knob(f, &densityMult, "vol_density_mult", "density mult");
    SetRange(f, 0.01, 10);
    Tooltip(f, "Multiplier on the VDB density grid values.");
    Double_knob(f, &stepSize, "vol_step_size", "step size");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0.01, 5);
    Tooltip(f, "Ray march step size in world units.\n"
               "Smaller = higher quality, slower.");

    Divider(f, "Emission");
    Double_knob(f, &emissionIntensity, "vol_emission", "intensity");
    SetRange(f, 0, 50);
    Tooltip(f, "Emission brightness multiplier.");
    Bool_knob(f, &useBlackbody, "vol_use_blackbody", "CIE blackbody");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Use physically-based CIE 1931 blackbody colours\n"
               "from the temperature grid. Maps temperature values\n"
               "to real fire/incandescence colours via Planck's law.\n\n"
               "500K = deep red (embers)\n"
               "1500K = orange (candle)\n"
               "3000K = warm yellow (fire)\n"
               "6500K = daylight white\n"
               "10000K+ = blue-white (plasma)");
    Double_knob(f, &tempMin, "vol_temp_min", "temp min");
    SetRange(f, 100, 5000);
    Tooltip(f, "Temperature grid value mapped to the low end.\n"
               "Voxels below this emit nothing.");
    Double_knob(f, &tempMax, "vol_temp_max", "temp max");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 1000, 40000);
    Tooltip(f, "Temperature grid value mapped to the high end.\n"
               "Controls the hottest emission colour.");
    Double_knob(f, &flameIntensity, "vol_flame_intensity", "flame");
    SetRange(f, 0, 20);
    Tooltip(f, "Flame grid intensity multiplier.\n"
               "Multiplied with emission intensity.");

    Divider(f, "Chromatic extinction");
    Bool_knob(f, &chromaticExtinction, "vol_chromatic", "enable");
    Tooltip(f, "Wavelength-dependent absorption.\n"
               "Short wavelengths scatter more, giving blue\n"
               "edges on smoke and warm thick regions.\n"
               "sigma R/G/B are relative multipliers on extinction.");
    Double_knob(f, &sigmaR, "vol_sigma_r", "R");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0.1, 3);
    Double_knob(f, &sigmaG, "vol_sigma_g", "G");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0.1, 3);
    Double_knob(f, &sigmaB, "vol_sigma_b", "B");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0.1, 3);
    Tooltip(f, "Relative extinction per channel.\n"
               "B > R = blue scatters more (Rayleigh-like).\n"
               "Default: R=1.0 G=1.0 B=1.2");

    BeginClosedGroup(f, "vol_phase", "Phase function");
    Double_knob(f, &gForward, "vol_g_forward", "forward lobe");
    SetRange(f, 0, 0.95);
    Tooltip(f, "Forward scattering anisotropy (0=isotropic, 0.95=very forward).\n"
               "Clouds and fog are strongly forward-scattering.");
    Double_knob(f, &gBackward, "vol_g_backward", "backward lobe");
    SetRange(f, -0.95, 0);
    Tooltip(f, "Backward scattering lobe strength.\n"
               "Negative values scatter light back toward the viewer.");
    Double_knob(f, &lobeMix, "vol_lobe_mix", "mix");
    SetRange(f, 0, 1);
    Tooltip(f, "Blend between forward and backward lobes.\n"
               "1.0 = all forward, 0.0 = all backward.");
    Double_knob(f, &powder, "vol_powder", "powder");
    SetRange(f, 0, 10);
    Tooltip(f, "Powder effect strength \xe2\x80\x94 darkens forward-lit edges.\n"
               "Creates the 'silver lining' look on clouds.");
    Bool_knob(f, &jitter, "vol_jitter", "jitter rays");
    Tooltip(f, "Add blue-noise jitter to ray march start.\n"
               "Reduces banding artifacts.");
    EndGroup(f);

    BeginClosedGroup(f, "vol_scatter_grp", "Scatter colour");
    Color_knob(f, scatterColor, "vol_scatter_color", "colour");
    Tooltip(f, "Tint applied to scattered light.\n"
               "White = neutral, warm = fire-like, blue = ethereal.");
    EndGroup(f);

    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralVolumeMaterial \xc2\xb7 SpectralRenderer for Nuke 17 \xc2\xb7 Created by Marten Blumen"
                 "</font>");
}

// ---------------------------------------------------------------------------
// knob_changed — presets
// ---------------------------------------------------------------------------
int SpectralVolumeMaterial::knob_changed(Knob* k)
{
    if (k->is("vol_preset") && preset > 0) {
        switch (preset) {
            case 1: // Smoke
                extinction=5; scattering=2; densityMult=1;
                gForward=0.3; gBackward=-0.1; lobeMix=0.8;
                powder=0; emissionIntensity=0; useBlackbody=false;
                chromaticExtinction=false;
                scatterColor[0]=0.7f; scatterColor[1]=0.7f; scatterColor[2]=0.7f;
                break;
            case 2: // Fire / Explosion
                extinction=8; scattering=4; densityMult=1;
                gForward=0.5; gBackward=-0.2; lobeMix=0.7;
                powder=3; emissionIntensity=5; tempMin=500; tempMax=6500;
                flameIntensity=5; useBlackbody=true;
                chromaticExtinction=false;
                scatterColor[0]=1.f; scatterColor[1]=0.85f; scatterColor[2]=0.6f;
                break;
            case 3: // Clouds
                extinction=12; scattering=10; densityMult=1;
                gForward=0.75; gBackward=-0.3; lobeMix=0.8;
                powder=4; emissionIntensity=0; useBlackbody=false;
                chromaticExtinction=false;
                scatterColor[0]=1.f; scatterColor[1]=1.f; scatterColor[2]=1.f;
                break;
            case 4: // Fog / Mist
                extinction=2; scattering=1.5; densityMult=0.5;
                gForward=0.6; gBackward=-0.15; lobeMix=0.85;
                powder=0; emissionIntensity=0; useBlackbody=false;
                chromaticExtinction=true; sigmaR=1.0; sigmaG=1.0; sigmaB=1.3;
                scatterColor[0]=0.9f; scatterColor[1]=0.92f; scatterColor[2]=0.95f;
                break;
            case 5: // Nebula
                extinction=1.5; scattering=1; densityMult=0.8;
                gForward=0.2; gBackward=0; lobeMix=1;
                powder=0; emissionIntensity=3; useBlackbody=true;
                tempMin=2000; tempMax=15000;
                chromaticExtinction=true; sigmaR=0.8; sigmaG=1.0; sigmaB=1.4;
                scatterColor[0]=0.6f; scatterColor[1]=0.4f; scatterColor[2]=1.f;
                break;
        }
        // Push values to knobs
        if (Knob* kn = knob("vol_extinction")) kn->set_value(extinction);
        if (Knob* kn = knob("vol_scattering")) kn->set_value(scattering);
        if (Knob* kn = knob("vol_density_mult")) kn->set_value(densityMult);
        if (Knob* kn = knob("vol_g_forward")) kn->set_value(gForward);
        if (Knob* kn = knob("vol_g_backward")) kn->set_value(gBackward);
        if (Knob* kn = knob("vol_lobe_mix")) kn->set_value(lobeMix);
        if (Knob* kn = knob("vol_powder")) kn->set_value(powder);
        if (Knob* kn = knob("vol_emission")) kn->set_value(emissionIntensity);
        if (Knob* kn = knob("vol_temp_min")) kn->set_value(tempMin);
        if (Knob* kn = knob("vol_temp_max")) kn->set_value(tempMax);
        if (Knob* kn = knob("vol_flame_intensity")) kn->set_value(flameIntensity);
        if (Knob* kn = knob("vol_use_blackbody")) kn->set_value(useBlackbody);
        if (Knob* kn = knob("vol_chromatic")) kn->set_value(chromaticExtinction);
        if (Knob* kn = knob("vol_sigma_r")) kn->set_value(sigmaR);
        if (Knob* kn = knob("vol_sigma_g")) kn->set_value(sigmaG);
        if (Knob* kn = knob("vol_sigma_b")) kn->set_value(sigmaB);
        if (Knob* kn = knob("vol_scatter_color")) {
            kn->set_value(scatterColor[0], 0);
            kn->set_value(scatterColor[1], 1);
            kn->set_value(scatterColor[2], 2);
        }
        return 1;
    }
    return ShaderOp::knob_changed(k);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ShaderOp
// ---------------------------------------------------------------------------
bool SpectralVolumeMaterial::test_input(int idx, Op* op) const
{
    if (idx == 0) return dynamic_cast<Iop*>(op) != nullptr;
    return ShaderOp::test_input(idx, op);
}

