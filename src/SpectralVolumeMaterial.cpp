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
           "Connect directly to GeoScene alongside SpectralVDBRead.\n\n"
           "RECOMMENDED CONNECTION\n"
           "  SpectralVDBRead -----+\n"
           "  SpectralVolumeMaterial --+-- GeoScene -- SpectralRender\n"
           "  SpectralEnvLight ----+\n\n"
           "IMPORTANT: Do NOT use GeoBind to assign this material.\n"
           "GeoBind breaks animated update propagation.\n"
           "Connect directly to GeoScene as a separate input.\n\n"
           "Without this node, SpectralRender uses its Volumes tab defaults.\n\n"
           "PRESETS (16)\n"
           "  Smoke: Light, Dense, Industrial\n"
           "  Fire: Campfire, Explosion, Pyroclastic\n"
           "  Clouds: Cumulus, Cirrus, Stratus, Storm\n"
           "  Atmosphere: Fog, Ground Mist, Haze, Dust Storm\n"
           "  Effects: Nebula, Underwater Caustic\n\n"
           "GRIDS\n"
           "  Grid selection (density, temperature, flame, colour) is on\n"
           "  SpectralVDBRead -- it discovers grids from the VDB file.\n\n"
           "Created by Marten Blumen\n"
           "github.com/bratgot/SpectralRenderer";
}

// ---------------------------------------------------------------------------
// Blackbody — Tanner Helland approximation
// ---------------------------------------------------------------------------
void SpectralVolumeMaterial::BlackbodyToRGB(float tempK, float& r, float& g, float& b)
{
    tempK = std::max(500.f, std::min(tempK, 40000.f));
    float t = tempK / 100.f;
    if (t <= 66.f) r = 1.f;
    else { float x = t - 60.f; r = 329.698727446f * std::pow(x, -0.1332047592f) / 255.f; }
    if (t <= 66.f) { float x = std::max(1.f, t); g = (99.4708025861f * std::log(x) - 161.1195681661f) / 255.f; }
    else { float x = t - 60.f; g = 288.1221695283f * std::pow(x, -0.0755148492f) / 255.f; }
    if (t >= 66.f) b = 1.f;
    else if (t <= 19.f) b = 0.f;
    else { float x = std::max(1.f, t - 10.f); b = (138.5177312231f * std::log(x) - 305.0447927307f) / 255.f; }
    r = std::max(0.f, std::min(r, 1.f));
    g = std::max(0.f, std::min(g, 1.f));
    b = std::max(0.f, std::min(b, 1.f));
}

// ---------------------------------------------------------------------------
// knobs
// ---------------------------------------------------------------------------
void SpectralVolumeMaterial::knobs(Knob_Callback f)
{
    // ─── Header ─────────────────────────────────────────────────────
    Text_knob(f, "<b>Volume Shader</b>");
    Newline(f);
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Defines the look of a volume. Connect alongside SpectralVDBRead<br>"
        "into GeoScene. Grid selection is on VDBRead (it discovers them)."
        "</font>"
    );

    // ─── Preset ─────────────────────────────────────────────────────
    Divider(f, "Preset");
    static const char* const presetNames[] = {
        "Custom",
        // Smoke
        "\xe2\x94\x80\xe2\x94\x80 Smoke \xe2\x94\x80\xe2\x94\x80",
        "Light Smoke", "Dense Smoke", "Industrial",
        // Fire
        "\xe2\x94\x80\xe2\x94\x80 Fire \xe2\x94\x80\xe2\x94\x80",
        "Candle", "Campfire", "Torch", "Wildfire", "Explosion", "Pyroclastic",
        // Nuclear
        "\xe2\x94\x80\xe2\x94\x80 Nuclear \xe2\x94\x80\xe2\x94\x80",
        "Little Boy", "Thermonuclear", "Rocket Engine",
        // Fantasy
        "\xe2\x94\x80\xe2\x94\x80 Fantasy \xe2\x94\x80\xe2\x94\x80",
        "Wizard Fireball", "Plasma Orb", "Cherenkov Reactor",
        // Clouds
        "\xe2\x94\x80\xe2\x94\x80 Clouds \xe2\x94\x80\xe2\x94\x80",
        "Cumulus", "Cirrus", "Stratus", "Storm Cloud",
        // Atmosphere
        "\xe2\x94\x80\xe2\x94\x80 Atmosphere \xe2\x94\x80\xe2\x94\x80",
        "Fog", "Ground Mist", "Haze", "Dust Storm",
        // Effects
        "\xe2\x94\x80\xe2\x94\x80 Effects \xe2\x94\x80\xe2\x94\x80",
        "Nebula", "Underwater Caustic",
        nullptr
    };
    Enumeration_knob(f, &preset, presetNames, "vol_preset", "");
    Tooltip(f, "One-click setup for common volume types.\n"
               "Adjusting any parameter switches to Custom.\n\n"
               "Smoke \xe2\x80\x94 varying density and absorption\n"
               "Fire \xe2\x80\x94 blackbody emission with temperature grids\n"
               "Nuclear \xe2\x80\x94 extreme temperature, core glow, shock wave\n"
               "Fantasy \xe2\x80\x94 magical fire, plasma, Cherenkov\n"
               "Clouds \xe2\x80\x94 high albedo, Mie scattering, powder effect\n"
               "Atmosphere \xe2\x80\x94 fog, mist, haze, dust\n"
               "Effects \xe2\x80\x94 nebula, underwater caustic");

    // ─── Density ────────────────────────────────────────────────────
    Divider(f, "Density");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Extinction = opacity per unit density. Scattering = brightness from light.<br>"
        "Albedo = scattering / extinction. High albedo = bright clouds."
        "</font>"
    );
    Newline(f);
    Double_knob(f, &extinction, "vol_extinction", "extinction");
    SetRange(f, 0.01, 50); SetFlags(f, Knob::LOG_SLIDER);
    Tooltip(f, "How quickly light is absorbed per unit density.\n"
               "0.1 = wisps. 1-3 = clouds. 5 = smoke. 10+ = dense.");
    Double_knob(f, &scattering, "vol_scattering", "scattering");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0.01, 50); SetFlags(f, Knob::LOG_SLIDER);
    Tooltip(f, "How bright the volume appears under lighting.\n"
               "Higher = brighter. 0 = pure absorption (dark).");
    Double_knob(f, &densityMult, "vol_density_mult", "density mult");
    SetRange(f, 0.01, 10); SetFlags(f, Knob::LOG_SLIDER);
    Tooltip(f, "Scales raw density values from VDB.\n"
               "0.1 = very thin. 1 = as-is. 3+ = dense.");
    Color_knob(f, scatterColor, "vol_scatter_color", "scatter colour");
    Tooltip(f, "Tint of scattered light inside the volume.\n"
               "White = neutral. Warm = fire. Blue = atmosphere.");

    // ─── Phase Function ─────────────────────────────────────────────
    Divider(f, "Phase Function");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Controls how light scatters. Dual-lobe HG is fast and versatile.<br>"
        "Approximate Mie is physically accurate for water droplets (clouds, fog)."
        "</font>"
    );
    Newline(f);
    static const char* const phaseModes[] = {
        "Dual-lobe HG", "Approximate Mie", nullptr
    };
    Enumeration_knob(f, &phaseMode, phaseModes, "vol_phase_mode", "mode");
    Tooltip(f, "Dual-lobe HG \xe2\x80\x94 fast, good for all volume types.\n"
               "Approximate Mie \xe2\x80\x94 Cornette-Shanks fit for water droplets.\n"
               "Use Mie for clouds, fog, and rain.");
    Double_knob(f, &mieDropletD, "vol_mie_droplet_d", "droplet");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0.1, 20);
    Tooltip(f, "Water droplet diameter in micrometres (Mie only).\n"
               "0.1 = aerosol/haze. 2 = cloud. 10 = large/drizzle.");
    Double_knob(f, &gForward, "vol_g_forward", "forward");
    SetRange(f, 0, 0.95);
    Tooltip(f, "Forward scatter lobe (HG g1).\n"
               "0 = isotropic. 0.85 = cloud. 0.95 = very forward.");
    Double_knob(f, &gBackward, "vol_g_backward", "backward");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, -0.95, 0);
    Tooltip(f, "Backward scatter lobe (HG g2).\n"
               "Creates rim/backlit glow. -0.1 = subtle. -0.3 = strong.");
    Double_knob(f, &lobeMix, "vol_lobe_mix", "mix");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0, 1);
    Tooltip(f, "Blend between forward and backward lobes.\n"
               "1 = all forward. 0 = all backward. 0.7-0.85 = typical.");
    Double_knob(f, &powder, "vol_powder", "powder");
    SetRange(f, 0, 10);
    Tooltip(f, "Interior brightening (Schneider & Vos 2015).\n"
               "Creates the 'silver lining' on clouds.\n"
               "0 = off. 2 = natural. 5 = dense explosion.");
    Double_knob(f, &gradientMix, "vol_gradient_mix", "gradient mix");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0, 1);
    Tooltip(f, "Blend HG phase with density-gradient Lambertian.\n"
               "Gives clouds sculpted billowing edges.\n"
               "0 = smoke/fire. 0.3 = clouds.\n"
               "CPU only -- ignored on GPU.");
    Bool_knob(f, &jitter, "vol_jitter", "ray jitter");
    Tooltip(f, "Per-pixel random step offset. Eliminates banding. Zero cost.");

    // ─── Emission ───────────────────────────────────────────────────
    Divider(f, "Emission");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Temperature and flame emission for fire rendering.<br>"
        "CIE blackbody uses Planck's law for physically accurate fire colours."
        "</font>"
    );
    Newline(f);
    Double_knob(f, &emissionIntensity, "vol_emission", "intensity");
    SetRange(f, 0, 50);
    Tooltip(f, "Emission brightness. 0 = off. 5 = fire. 15 = explosion.");
    Bool_knob(f, &useBlackbody, "vol_use_blackbody", "CIE blackbody");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Physically-based fire colours from temperature grid.\n"
               "500K = embers. 1500K = candle. 3000K = fire. 6500K = white.");
    Double_knob(f, &tempMin, "vol_temp_min", "temp range");
    SetRange(f, 0, 5000);
    Tooltip(f, "Temperature at zero emission (Kelvin).");
    Double_knob(f, &tempMax, "vol_temp_max", "");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 500, 40000);
    Tooltip(f, "Temperature at peak emission (Kelvin).");
    Double_knob(f, &flameIntensity, "vol_flame_intensity", "flame");
    SetRange(f, 0, 20);
    Tooltip(f, "Flame grid brightness. Additive on top of temperature.");
    Double_knob(f, &flameOpacity, "vol_flame_opacity", "burn");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 1);
    Tooltip(f, "Flame burns away density.\n"
               "0 = none (flame glows through smoke).\n"
               "0.5 = partial burnaway.\n"
               "1 = full (fire core is transparent).");
    Double_knob(f, &flameTempMin, "vol_flame_temp_min", "flame temp");
    SetRange(f, 500, 5000);
    Tooltip(f, "Flame grid minimum temperature (K).\n"
               "Maps flame=0 to this temperature.\n"
               "1200 = embers. 1500 = candle.");
    Double_knob(f, &flameTempMax, "vol_flame_temp_max", "");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 1000, 10000);
    Tooltip(f, "Flame grid maximum temperature (K).\n"
               "Maps flame=1 to this temperature.\n"
               "3500 = fire. 6500 = welding arc.");

    Double_knob(f, &coreGlow, "vol_core_glow", "core glow");
    SetRange(f, 0, 10);
    Tooltip(f, "Dense core emission. Adds blackbody glow\n"
               "in high-density regions (density > 0.3).\n"
               "Good for explosion interiors.");
    Double_knob(f, &coreTemp, "vol_core_temp", "");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 1000, 10000);
    Tooltip(f, "Core emission temperature (K).\n"
               "4000 = warm orange. 6500 = white. 10000 = blue-white.");

    BeginClosedGroup(f, "vol_cherenkov_grp", "Cherenkov radiation");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Blue glow from charged particles exceeding the speed of light<br>"
            "in a medium (nuclear reactors, cosmic phenomena)."
            "</font>"
        );
        Newline(f);
        Bool_knob(f, &cherenkov, "vol_cherenkov", "enable");
        Double_knob(f, &cherenkovStrength, "vol_cherenkov_strength", "strength");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 100); SetFlags(f, Knob::LOG_SLIDER);
        Double_knob(f, &cherenkovThreshold, "vol_cherenkov_threshold", "threshold");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0.001, 1); SetFlags(f, Knob::LOG_SLIDER);
        Tooltip(f, "Density threshold for Cherenkov activation.\n"
                   "Lower = more widespread glow.");
    }
    EndGroup(f);

    // ─── Chromatic Extinction ───────────────────────────────────────
    BeginClosedGroup(f, "vol_chrom_grp", "Chromatic Extinction (cpu)");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Wavelength-dependent absorption. Blue scatters more = blue haze edges."
            "</font>"
        );
        Newline(f);
        Bool_knob(f, &chromaticExtinction, "vol_chromatic", "enable");
        Tooltip(f, "Enable wavelength-dependent extinction.\n"
                   "Shorter wavelengths scatter more, creating blue haze on edges\n"
                   "and warm tones in thick regions (Rayleigh-like effect).\n"
                   "Requires spectral volumes mode on CPU.");
        Double_knob(f, &sigmaR, "vol_sigma_r", "R");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0.1, 3);
        Tooltip(f, "Red channel extinction multiplier.\n"
                   "1.0 = normal. Lower = red light passes through more easily.");
        Double_knob(f, &sigmaG, "vol_sigma_g", "G");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0.1, 3);
        Tooltip(f, "Green channel extinction multiplier.\n"
                   "1.0 = normal. Typically between R and B values.");
        Double_knob(f, &sigmaB, "vol_sigma_b", "B");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0.1, 3);
        Tooltip(f, "Blue channel extinction multiplier.\n"
                   "Higher than R = blue scatters more (Rayleigh-like).\n"
                   "Default 1.2 gives subtle blue haze on smoke edges.");
    }
    EndGroup(f);

    // ─── Procedural Noise ───────────────────────────────────────────
    BeginClosedGroup(f, "vol_noise_grp", "Procedural Detail Noise");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "fBm noise adds detail beyond VDB resolution at render time.<br>"
            "Use a LOD preset or tune manually. Enable 'normalize to bbox'<br>"
            "for consistent look across different volume sizes."
            "</font>"
        );
        Newline(f);
        static const char* const noiseLOD[] = {
            "Custom", "Subtle", "Natural", "Detailed", "Extreme", "Wispy Edges", nullptr
        };
        Enumeration_knob(f, &noiseLodPreset, noiseLOD, "vol_noise_lod", "detail level");
        Tooltip(f, "Quick-set noise parameters by detail level.\n\n"
                   "Subtle -- light wisps at edges, barely visible.\n"
                   "Natural -- realistic breakup, good default.\n"
                   "Detailed -- strong visible noise, fine features.\n"
                   "Extreme -- heavy noise, maximum breakup.\n"
                   "Wispy Edges -- thin tendrils at volume boundary.");
        Bool_knob(f, &noiseEnable, "vol_noise_enable", "enable");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Add fBm noise to density at render time.\n"
                   "Adds detail beyond VDB grid resolution.\n"
                   "World-space -- no UV unwrap needed.");
        Bool_knob(f, &noiseNormalize, "vol_noise_normalize", "normalize to bbox");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Scale noise frequency relative to the volume bounding box.\n"
                   "ON: noise looks the same regardless of volume world size.\n"
                   "OFF: noise is in absolute world units.\n"
                   "Enable for large clouds where default scale is too coarse.");
        Double_knob(f, &noiseScale, "vol_noise_scale", "scale"); SetRange(f, 0.1, 20);
        Tooltip(f, "Noise frequency.\n"
                   "With normalize ON: 4 = four noise cycles across the bbox.\n"
                   "With normalize OFF: world-space frequency.\n"
                   "Lower = broader features. Higher = finer detail.");
        Double_knob(f, &noiseStrength, "vol_noise_strength", "strength");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 1);
        Tooltip(f, "How much noise affects density.\n"
                   "0 = none. 0.3 = natural wisps. 0.5 = strong. 1 = extreme.\n"
                   "Start at 0.2-0.3 and increase to taste.");
        Int_knob(f, &noiseOctaves, "vol_noise_octaves", "octaves"); SetRange(f, 1, 6);
        Tooltip(f, "Layers of noise at increasing frequency.\n"
                   "1 = smooth blobs. 3 = natural detail. 6 = maximum detail.\n"
                   "Each octave adds render cost. 3-4 is a good balance.");
        Double_knob(f, &noiseRoughness, "vol_noise_roughness", "roughness");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 1);
        Tooltip(f, "How much detail each octave adds.\n"
                   "0 = smooth (only first octave matters).\n"
                   "0.5 = natural. 1 = rough/gritty.");
    }
    EndGroup(f);

    // ─── Grid Mixer ──────────────────────────────────────────────────
    BeginClosedGroup(f, "vol_grid_mix_grp", "Grid Mixer");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Fade VDB grids on and off. 0 = disabled, 1 = full strength."
            "</font>"
        );
        Newline(f);
        Double_knob(f, &densityMix, "vol_density_mix", "density");
        SetRange(f, 0, 1);
        Tooltip(f, "Fade density grid.\n0 = no density (invisible).\n1 = full density.");
        Double_knob(f, &tempMix, "vol_temp_mix", "temperature");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 1);
        Tooltip(f, "Fade temperature grid.\n0 = no temperature emission.\n1 = full temperature.");
        Double_knob(f, &flameMix, "vol_flame_mix", "flame");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 1);
        Tooltip(f, "Fade flame grid.\n0 = no flame emission.\n1 = full flame.");
    }
    EndGroup(f);

    // ─── Multiple Scattering ────────────────────────────────────────
    BeginClosedGroup(f, "vol_ms_grp", "Multiple Scattering (cpu)");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Wrenninge 2015: infinite-bounce approximation at near-zero cost."
            "</font>"
        );
        Newline(f);
        Bool_knob(f, &msApprox, "vol_ms_approx", "analytical MS");
        Tooltip(f, "Analytical infinite-bounce multiple scattering.\n"
                   "Brightens dense volume interiors at near-zero cost.\n"
                   "Based on Wrenninge 2015. Leave ON for natural-looking\n"
                   "clouds and smoke. OFF for purely artistic control.");
        Color_knob(f, msTint, "vol_ms_tint", "tint");
        Tooltip(f, "Colour tint on multi-scatter contribution.\n"
                   "Default warm (1, 0.97, 0.95) adds subtle warmth.\n"
                   "Try (0.95, 0.97, 1.0) for cold/overcast look.\n"
                   "White (1, 1, 1) = neutral.");
    }
    EndGroup(f);

    // ─── Footer ─────────────────────────────────────────────────────
    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralVolumeMaterial \xc2\xb7 SpectralRenderer for Nuke 17"
                 "</font>");
}

// ---------------------------------------------------------------------------
// knob_changed — 16 presets
// ---------------------------------------------------------------------------
int SpectralVolumeMaterial::knob_changed(Knob* k)
{
    if (k->is("vol_preset") && preset > 0) {
        struct P { double ext,scat,dens,gF,gB,lM,pow,emI,tMin,tMax,flI;
                   float scR,scG,scB; int phase; double mieD;
                   double flOp,flTMin,flTMax,cGlow,cTemp; bool cher; double cherStr,cherThr; };
        //                         ext  scat dens  gF    gB    lM   pow emI  tMin  tMax  flI   scR   scG   scB  ph  mie  flOp flTMin flTMax cGlow cTemp  cher cherStr cherThr
        static const P sp[] = {
            {}, // 0: Custom

            {}, // 1: ── Smoke ──
            // 2: Light Smoke — wispy, translucent
            {1.5, 0.8, 1,   0.3, -0.1,  0.8,  0,  0,    0,    0,    0,  0.7f, 0.7f, 0.7f,  0, 0,    0, 1200,3500, 0, 4000, false,0,0},
            // 3: Dense Smoke — thick, dark
            {4,   1.5, 1.5, 0.25,-0.15, 0.75, 0,  0,    0,    0,    0,  0.5f, 0.5f, 0.5f,  0, 0,    0, 1200,3500, 0, 4000, false,0,0},
            // 4: Industrial — sooty, warm tint
            {6,   2,   2,   0.2, -0.1,  0.7,  0,  0,    0,    0,    0,  0.4f, 0.38f,0.35f, 0, 0,    0, 1200,3500, 0, 4000, false,0,0},

            {}, // 5: ── Fire ──
            // 6: Candle
            {0.6, 0.4, 0.8, 0.35,-0.1,  0.75, 0.5, 3, 600, 1800,  4,  1.f,  0.95f,0.85f, 0, 0,   0.2, 900,1800, 0.5,1800, false,0,0},
            // 7: Campfire
            {1,   0.8, 1,   0.4, -0.15, 0.7,  1,  5,  800, 2500,   8,  1.f,  0.9f, 0.7f,  0, 0,   0.3,1200,2500, 1, 3000, false,0,0},
            // 8: Torch
            {1.2, 0.9, 1.2, 0.5, -0.15, 0.7,  1.5, 8, 700, 3000, 10,  1.f,  0.85f,0.6f,  0, 0,   0.4,1000,3000, 2, 3500, false,0,0},
            // 9: Wildfire
            {3,   1.5, 1.8, 0.5, -0.2,  0.7,  2, 12,  400, 4000,  15,  0.9f, 0.75f,0.5f,  0, 0,   0.5,1000,4000, 3, 4500, false,0,0},
            // 10: Explosion
            {2,   1.5, 1.2, 0.6, -0.25, 0.75, 3, 15,  300, 6000,  20,  1.f,  0.85f,0.6f,  0, 0,   0.6,1500,5000, 5, 6000, false,0,0},
            // 11: Pyroclastic
            {4,   2.5, 2,   0.7, -0.3,  0.8,  4, 20,  200, 8000,  25,  0.85f,0.7f, 0.5f,  0, 0,   0.8,1000,8000, 8, 7000, false,0,0},

            {}, // 12: ── Nuclear ──
            // 13: Little Boy
            {5,   3,   2.5, 0.8, -0.3,  0.85, 5, 40,  500,15000,  30,  1.f,  0.9f, 0.7f,  0, 0,   0.9,2000,15000,15,10000,false,0,0},
            // 14: Thermonuclear
            {8,   4,   3,   0.85,-0.35, 0.9,  6, 80,  1000,50000, 50,  1.f,  0.95f,0.9f,  0, 0,   1.0,5000,50000,50,30000,false,0,0},
            // 15: Rocket Engine
            {2.5, 2,   1.5, 0.9, -0.1,  0.9,  2, 25,  800,4500,  20,  0.95f,0.9f, 0.8f,  0, 0,   0.7,1500,4500, 4, 5000, false,0,0},

            {}, // 16: ── Fantasy ──
            // 17: Wizard Fireball
            {1.5, 1.2, 1,   0.5, -0.2,  0.7,  2, 20,  500,8000,  15,  0.7f, 0.4f, 1.f,   0, 0,   0.5,1500,8000, 8, 12000,false,0,0},
            // 18: Plasma Orb
            {1,   0.9, 0.8, 0.3, -0.05, 0.9,  1, 12, 3000,20000,  0,  0.4f, 0.6f, 1.f,   0, 0,   0.3,5000,20000,6, 15000,false,0,0},
            // 19: Cherenkov Reactor
            {0.8, 0.6, 0.6, 0.2,  0,    1,    0,  3, 2000,8000,   0,  0.3f, 0.5f, 0.9f,  0, 0,    0, 2000,8000, 1, 6000, true, 5, 0.3},

            {}, // 20: ── Clouds ──
            // 21: Cumulus
            {1.2, 1.1, 1,   0.85,-0.1,  0.85, 3,  0,    0,    0,    0,  1.f,  1.f,  1.f,   1, 8,    0, 1200,3500, 0, 4000, false,0,0},
            // 22: Cirrus
            {0.4, 0.38,0.6, 0.75,-0.05, 0.9,  1,  0,    0,    0,    0,  1.f,  1.f,  1.f,   1, 1,    0, 1200,3500, 0, 4000, false,0,0},
            // 23: Stratus
            {1.8, 1.7, 1,   0.85,-0.1,  0.85, 2,  0,    0,    0,    0,  0.98f,0.98f,1.f,   1, 5,    0, 1200,3500, 0, 4000, false,0,0},
            // 24: Storm Cloud
            {3,   2.8, 1.5, 0.87,-0.25, 0.8,  4,  0,    0,    0,    0,  0.9f, 0.9f, 0.92f, 1, 10,   0, 1200,3500, 0, 4000, false,0,0},

            {}, // 25: ── Atmosphere ──
            // 26: Fog
            {0.8, 0.7, 0.5, 0.6, -0.15, 0.85, 0,  0,    0,    0,    0,  0.92f,0.94f,0.97f, 1, 3,    0, 1200,3500, 0, 4000, false,0,0},
            // 27: Ground Mist
            {0.8, 0.75,0.3, 0.5, -0.1,  0.85, 0,  0,    0,    0,    0,  0.95f,0.96f,1.f,   1, 2,    0, 1200,3500, 0, 4000, false,0,0},
            // 28: Haze
            {0.3, 0.28,0.2, 0.7, -0.05, 0.9,  0,  0,    0,    0,    0,  0.9f, 0.92f,0.98f, 1, 0.5,  0, 1200,3500, 0, 4000, false,0,0},
            // 29: Dust Storm
            {2.5, 1,   1.5, 0.4, -0.1,  0.6,  0,  0,    0,    0,    0,  0.85f,0.75f,0.6f,  0, 0,    0, 1200,3500, 0, 4000, false,0,0},

            {}, // 30: ── Effects ──
            // 31: Nebula
            {0.8, 0.4, 0.8, 0.2,  0,    1,    0,  5, 2000,15000,   0,  0.6f, 0.4f, 1.f,   0, 0,    0, 1200,3500, 0.5,8000, false,0,0},
            // 32: Underwater Caustic
            {0.5, 0.48,0.4, 0.85,-0.1,  0.9,  1,  0,    0,    0,    0,  0.7f, 0.9f, 1.f,   1, 4,    0, 1200,3500, 0, 4000, false,0,0},
        };
        int n = sizeof(sp)/sizeof(sp[0]);
        if (preset < n) {
            const auto& s = sp[preset];
            // Skip section headers (empty entries)
            if (s.ext == 0 && s.scat == 0 && s.dens == 0) return 1;
            extinction=s.ext; scattering=s.scat; densityMult=s.dens;
            gForward=s.gF; gBackward=s.gB; lobeMix=s.lM;
            powder=s.pow; emissionIntensity=s.emI;
            tempMin=s.tMin; tempMax=s.tMax; flameIntensity=s.flI;
            scatterColor[0]=s.scR; scatterColor[1]=s.scG; scatterColor[2]=s.scB;
            phaseMode=s.phase; mieDropletD=s.mieD;
            // Phase 17: fire params
            flameOpacity=s.flOp; flameTempMin=s.flTMin; flameTempMax=s.flTMax;
            coreGlow=s.cGlow; coreTemp=s.cTemp;
            cherenkov=s.cher; cherenkovStrength=s.cherStr; cherenkovThreshold=s.cherThr;
            // Fire presets enable blackbody
            useBlackbody = (s.emI > 0);
            // Push to knobs
            if (Knob* kn = knob("vol_extinction"))   kn->set_value(extinction);
            if (Knob* kn = knob("vol_scattering"))   kn->set_value(scattering);
            if (Knob* kn = knob("vol_density_mult")) kn->set_value(densityMult);
            if (Knob* kn = knob("vol_g_forward"))    kn->set_value(gForward);
            if (Knob* kn = knob("vol_g_backward"))   kn->set_value(gBackward);
            if (Knob* kn = knob("vol_lobe_mix"))     kn->set_value(lobeMix);
            if (Knob* kn = knob("vol_powder"))       kn->set_value(powder);
            if (Knob* kn = knob("vol_emission"))     kn->set_value(emissionIntensity);
            if (Knob* kn = knob("vol_temp_min"))     kn->set_value(tempMin);
            if (Knob* kn = knob("vol_temp_max"))     kn->set_value(tempMax);
            if (Knob* kn = knob("vol_flame_intensity")) kn->set_value(flameIntensity);
            if (Knob* kn = knob("vol_use_blackbody"))   kn->set_value(useBlackbody);
            if (Knob* kn = knob("vol_phase_mode"))   kn->set_value(phaseMode);
            if (Knob* kn = knob("vol_mie_droplet_d")) kn->set_value(mieDropletD);
            if (Knob* kn = knob("vol_scatter_color")) {
                kn->set_value(scatterColor[0], 0);
                kn->set_value(scatterColor[1], 1);
                kn->set_value(scatterColor[2], 2);
            }
            // Phase 17 fire knobs
            if (Knob* kn = knob("vol_flame_opacity"))    kn->set_value(flameOpacity);
            if (Knob* kn = knob("vol_flame_temp_min"))   kn->set_value(flameTempMin);
            if (Knob* kn = knob("vol_flame_temp_max"))   kn->set_value(flameTempMax);
            if (Knob* kn = knob("vol_core_glow"))        kn->set_value(coreGlow);
            if (Knob* kn = knob("vol_core_temp"))        kn->set_value(coreTemp);
            if (Knob* kn = knob("vol_cherenkov"))        kn->set_value(cherenkov);
            if (Knob* kn = knob("vol_cherenkov_strength")) kn->set_value(cherenkovStrength);
            if (Knob* kn = knob("vol_cherenkov_threshold")) kn->set_value(cherenkovThreshold);
        }
        return 1;
    }

    // Noise LOD presets
    if (k->is("vol_noise_lod") && noiseLodPreset > 0) {
        //                    scale  strength octaves roughness normalize
        struct NP { double s,st; int o; double r; bool n; };
        static const NP np[] = {
            {},
            {2,   0.15, 2, 0.4, true},   // Subtle
            {4,   0.3,  3, 0.5, true},   // Natural
            {6,   0.45, 4, 0.6, true},   // Detailed
            {10,  0.6,  6, 0.7, true},   // Extreme
            {8,   0.5,  5, 0.8, false},  // Wispy Edges (world-space, high roughness)
        };
        if (noiseLodPreset < 6) {
            const auto& p = np[noiseLodPreset];
            noiseEnable = true;
            noiseScale = p.s; noiseStrength = p.st;
            noiseOctaves = p.o; noiseRoughness = p.r;
            noiseNormalize = p.n;
            if (Knob* kn = knob("vol_noise_enable"))    kn->set_value(1);
            if (Knob* kn = knob("vol_noise_scale"))     kn->set_value(p.s);
            if (Knob* kn = knob("vol_noise_strength"))  kn->set_value(p.st);
            if (Knob* kn = knob("vol_noise_octaves"))   kn->set_value(p.o);
            if (Knob* kn = knob("vol_noise_roughness")) kn->set_value(p.r);
            if (Knob* kn = knob("vol_noise_normalize")) kn->set_value(p.n ? 1 : 0);
        }
        return 1;
    }

    return ShaderOp::knob_changed(k);
}

bool SpectralVolumeMaterial::test_input(int idx, Op* op) const
{
    if (idx == 0) return dynamic_cast<Iop*>(op) != nullptr;
    return ShaderOp::test_input(idx, op);
}
