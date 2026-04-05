// SpectralStudioLight — three-point studio lighting for SpectralRender
// Created by Marten Blumen

#include "SpectralStudioLight.h"
#include <DDImage/Knobs.h>
#include <cmath>

using namespace DD::Image;

const char* const SpectralStudioLight::CLASS = "SpectralStudioLight";
static Op* build(Node* node) { return new SpectralStudioLight(node); }
const Op::Description SpectralStudioLight::description(CLASS, build);

SpectralStudioLight::SpectralStudioLight(Node* node) : ShaderOp(node) {}

const char* SpectralStudioLight::node_help() const
{
    return "SpectralStudioLight \xe2\x80\x94 Studio Lighting\n\n"
           "Three-point studio rig: key, fill, and rim lights.\n"
           "Presets for portrait, product, dramatic, and softbox setups.\n\n"
           "CONNECTION\n"
           "  SpectralStudioLight -> GeoScene -> SpectralRender (scn)\n\n"
           "KEY LIGHT\n"
           "  Main directional light. Warm tint, controllable angle.\n"
           "  Shadow softness simulates area light sources.\n\n"
           "FILL LIGHT\n"
           "  Opposite the key. Cool tint to lift shadows.\n"
           "  Intensity relative to key (fill ratio).\n\n"
           "RIM LIGHT\n"
           "  Behind the subject. Creates edge separation.\n\n"
           "Created by Marten Blumen\n"
           "github.com/bratgot/SpectralRenderer";
}

void SpectralStudioLight::knobs(Knob_Callback f)
{
    // ─── Header ─────────────────────────────────────────────────────
    Text_knob(f, "<b>Studio Light</b>");
    Newline(f);
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Three-point studio rig: key, fill, rim.<br>"
        "Connect to GeoScene alongside your volume or geometry."
        "</font>"
    );

    // ─── Preset ─────────────────────────────────────────────────────
    Divider(f, "Preset");
    static const char* const presetNames[] = {
        "Off", "Portrait", "Product", "Dramatic", "Softbox", nullptr
    };
    Enumeration_knob(f, &studioPreset, presetNames, "studio_preset", "");
    Tooltip(f, "Studio lighting presets.\n\n"
               "Portrait: soft key 45deg, moderate fill, subtle rim.\n"
               "  Best for: character lighting, close-ups.\n"
               "Product: bright key 60deg, low fill, strong rim.\n"
               "  Best for: product shots, hard surfaces.\n"
               "Dramatic: steep key 80deg, near-zero fill, intense rim.\n"
               "  Best for: moody scenes, noir look.\n"
               "Softbox: broad key 30deg, high fill, gentle rim.\n"
               "  Best for: beauty, soft wrapping light.");
    Double_knob(f, &mix, "studio_mix", "mix");
    SetRange(f, 0, 1);
    Tooltip(f, "Overall studio light contribution.\n"
               "0 = off. 1 = full strength.\n"
               "Blend with environment lighting.");

    // ─── Key Light ──────────────────────────────────────────────────
    Divider(f, "Key Light");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Main light source. Warm tint creates natural-looking illumination."
        "</font>"
    );
    Newline(f);
    Double_knob(f, &keyIntensity, "key_intensity", "intensity");
    SetRange(f, 0, 20);
    Tooltip(f, "Key light brightness.\n"
               "3 = soft. 5 = standard. 10+ = harsh.");
    Double_knob(f, &keyElevation, "key_elevation", "elevation");
    SetRange(f, 0, 90);
    Tooltip(f, "Key light angle above horizon.\n"
               "35 = portrait. 60 = product. 80 = dramatic top-down.");
    Double_knob(f, &keyAzimuth, "key_azimuth", "azimuth");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0, 360);
    Tooltip(f, "Key light compass direction.\n"
               "45 = classic portrait (front-left). 90 = side. 180 = back.");
    Color_knob(f, keyColor, "key_color", "colour");
    Tooltip(f, "Key light tint. Slightly warm (1, 0.97, 0.92) is natural.\n"
               "Pure white for neutral. Orange for candlelight.");

    // ─── Fill Light ─────────────────────────────────────────────────
    Divider(f, "Fill Light");
    Double_knob(f, &fillRatio, "fill_ratio", "fill ratio");
    SetRange(f, 0, 1);
    Tooltip(f, "Fill brightness relative to key.\n"
               "0 = no fill (harsh shadows). 0.4 = natural.\n"
               "0.8 = very soft. 1 = flat (no shadows).");
    Color_knob(f, fillColor, "fill_color", "colour");
    Tooltip(f, "Fill light tint. Cool blue (0.85, 0.9, 1.0) lifts shadows\n"
               "naturally. White for neutral fill.");

    // ─── Rim Light ──────────────────────────────────────────────────
    Divider(f, "Rim Light");
    Double_knob(f, &rimIntensity, "rim_intensity", "intensity");
    SetRange(f, 0, 10);
    Tooltip(f, "Rim/backlight brightness.\n"
               "Creates edge separation from background.\n"
               "0 = off. 2 = subtle. 5 = strong. 10 = dramatic.");
    Color_knob(f, rimColor, "rim_color", "colour");
    Tooltip(f, "Rim light tint. White is typical.\n"
               "Match env colour for natural integration.");

    // ─── Shadow ─────────────────────────────────────────────────────
    Divider(f, "Shadow");
    Double_knob(f, &shadowSoftness, "shadow_softness", "softness");
    SetRange(f, 0, 1);
    Tooltip(f, "Shadow edge quality. Simulates area light sources.\n"
               "0 = razor sharp (point light)\n"
               "0.2 = slightly soft (small area)\n"
               "0.5 = medium soft (overcast feel)\n"
               "1.0 = very diffuse (large softbox)");

    // ─── Footer ─────────────────────────────────────────────────────
    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralStudioLight \xc2\xb7 SpectralRenderer for Nuke 17"
                 "</font>");
}

int SpectralStudioLight::knob_changed(Knob* k)
{
    if (k->is("studio_preset") && studioPreset > 0) {
        struct SP { double kI,kE,kA,fR,rI,ss; };
        static const SP presets[] = {
            {},
            {5,  35, 45, 0.4, 2,   0.3},  // Portrait
            {8,  60, 45, 0.2, 4,   0.1},  // Product
            {6,  80, 30, 0.05,5,   0.0},  // Dramatic
            {4,  30, 45, 0.6, 1.5, 0.5},  // Softbox
        };
        int n = sizeof(presets)/sizeof(presets[0]);
        if (studioPreset < n) {
            const auto& s = presets[studioPreset];
            keyIntensity=s.kI; keyElevation=s.kE; keyAzimuth=s.kA;
            fillRatio=s.fR; rimIntensity=s.rI; shadowSoftness=s.ss;
            if (Knob* kn = knob("key_intensity")) kn->set_value(keyIntensity);
            if (Knob* kn = knob("key_elevation")) kn->set_value(keyElevation);
            if (Knob* kn = knob("key_azimuth")) kn->set_value(keyAzimuth);
            if (Knob* kn = knob("fill_ratio")) kn->set_value(fillRatio);
            if (Knob* kn = knob("rim_intensity")) kn->set_value(rimIntensity);
            if (Knob* kn = knob("shadow_softness")) kn->set_value(shadowSoftness);
        }
        return 1;
    }
    return ShaderOp::knob_changed(k);
}
