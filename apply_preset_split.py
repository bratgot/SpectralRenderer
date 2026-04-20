#!/usr/bin/env python3
"""
apply_preset_split.py -- split the single large preset enum into 9
per-category enums, AND fix the preset-UI-refresh bug in the same pass.

Why:
  1. The old preset Enumeration_knob had ~43 entries including dividers
     made of box-drawing chars, visually noisy, and the dividers were
     selectable (clicking them did nothing which felt broken).
  2. When a preset was applied, the switch() would write to member
     variables directly, THEN call knob->set_value(). Nuke's change
     detection then saw the bound variable already equalled the new
     value and skipped the UI redraw, so the sliders didn't update.

What changes:

  -  Replace the single Preset enum with 9 category enums (dielectrics,
     metals, organic, spectral, creative, measured-metals, fabrics,
     coatings). Custom is just "leave all enums at (none)".
  -  Layout: 3 per horizontal row for the first 3 rows, 2 for the last.
  -  New _ApplyPresetV2(category, preset) uses the correct refresh
     pattern: fill a local PresetValues struct, then call set_value()
     for every knob, never writing the member directly. Nuke sees
     old_value != new_value and invalidates the UI properly.
  -  Old _spectralPreset int member + _ApplyPreset + kSpectralPresetNames
     kept so legacy .nk files still load (old enum knob now INVISIBLE).

Files touched: src/SpectralSurfaceOp.h, src/SpectralSurfaceOp.cpp
Idempotent, CRLF-safe, backs up to .bak_presetsplit.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  HEADER: new category members + new apply method
# ============================================================================

H_OLD = """    float _sssColor[3] = {0.f, 0.f, 0.f};
    float _sssRadius = 0.f;
"""

H_NEW = """    float _sssColor[3] = {0.f, 0.f, 0.f};
    float _sssRadius = 0.f;

    // Per-category preset selectors (new split UI). Each is an index
    // into the category's small preset list. 0 = "(none)". Legacy
    // _spectralPreset kept for backward compatibility with old .nk
    // files.
    int _presetDielectric = 0;
    int _presetMetal      = 0;
    int _presetOrganic    = 0;
    int _presetSpectral   = 0;
    int _presetCreative   = 0;
    int _presetMetalRgl   = 0;
    int _presetFabric     = 0;
    int _presetCoating    = 0;

    // _ApplyPresetV2 replaces the old _ApplyPreset for the new split
    // UI. Key difference: it writes through Knob::set_value ONLY,
    // never assigning members directly. This ensures Nuke's change
    // detection sees the value change and refreshes the panel widget.
    void _ApplyPresetV2(int category, int preset);
"""


# ============================================================================
#  CPP: insert the new category name arrays + Enumeration_knobs, and
#       make the old preset enum INVISIBLE (but still present so legacy
#       .nk files don't complain).
# ============================================================================

# Old: single Enumeration_knob with huge list
# New: old knob marked INVISIBLE + 9 new category enums

CPP_KNOBS_OLD = """    Enumeration_knob(f, &_spectralPreset, kSpectralPresetNames, "preset", "Preset");
    Tooltip(f, "Material presets — physically accurate starting points.\\n\\n"
               "DIELECTRICS: transparent/refractive (set opacity low)\\n"
               "  glass, diamond, water, ruby\\n\\n"
               "METALS: spectral complex IOR from Palik measured data\\n"
               "  copper, gold, silver, aluminium\\n\\n"
               "ORGANIC: everyday surfaces with SSS and texture\\n"
               "  skin (has SSS), jade (has SSS), wood, paper\\n\\n"
               "SPECTRAL: features only possible in spectral renderers\\n"
               "  CD/DVD (diffraction), soap bubble (thin-film)\\n"
               "  highlighter (fluorescence UV->green)\\n\\n"
               "CREATIVE: fictional/artistic materials\\n"
               "  kryptonite, bioluminescence, plasma\\n\\n"
               "MEASURED (RGL): approximations of BRDF measurements\\n"
               "  from EPFL Realistic Graphics Lab database.\\n"
               "  Metals, fabrics, car paints, porcelain.\\n\\n"
               "Select 'custom' to set all values manually.");
    Text_knob(f,
        "<font color='#888' size='-1'>"
        "Tip: presets reset all properties. Tweak after selecting."
        "</font>"
    );
"""

CPP_KNOBS_NEW = """    // Legacy single-preset enum, kept INVISIBLE so old .nk files load
    // without knob warnings. All new work goes through the per-category
    // enums below.
    Enumeration_knob(f, &_spectralPreset, kSpectralPresetNames, "preset", "Preset");
    SetFlags(f, Knob::INVISIBLE);

    // Per-category preset enums -- compact layout, 3 per row top row,
    // 3 middle row, 2 bottom row. Each starts with "(none)" so picking
    // nothing leaves the current values alone.
    {
        static const char* const kDielectric[] = {
            "(none)", "glass", "diamond", "water", "ruby", nullptr
        };
        Enumeration_knob(f, &_presetDielectric, kDielectric,
                         "preset_dielectric", "dielectrics");
        Tooltip(f, "Transparent / refractive materials. Glass, diamond, water,\\n"
                   "ruby. Low opacity triggers refraction (enable transmission bounces\\n"
                   "in the render settings for full effect).");
    }
    {
        static const char* const kMetal[] = {
            "(none)", "copper", "gold", "silver", "aluminium", nullptr
        };
        Enumeration_knob(f, &_presetMetal, kMetal,
                         "preset_metal", "metals");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Measured metal presets using spectral complex IOR (n,k)\\n"
                   "tables from Palik. Copper, gold, silver, aluminium.");
    }
    {
        static const char* const kOrganic[] = {
            "(none)", "skin", "wood", "white paper", "concrete", "rubber", "jade", nullptr
        };
        Enumeration_knob(f, &_presetOrganic, kOrganic,
                         "preset_organic", "organic");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Everyday surfaces. Skin and jade use subsurface scattering.\\n"
                   "Rubber / concrete / paper are diffuse with tuned roughness.");
    }
    {
        static const char* const kSpectral[] = {
            "(none)", "CD/DVD", "soap bubble", "highlighter", nullptr
        };
        Enumeration_knob(f, &_presetSpectral, kSpectral,
                         "preset_spectral", "spectral");
        Tooltip(f, "Effects only possible in a spectral renderer:\\n"
                   "  CD/DVD     -- diffraction grating\\n"
                   "  soap bubble -- thin-film interference\\n"
                   "  highlighter -- fluorescence (UV->green)");
    }
    {
        static const char* const kCreative[] = {
            "(none)", "kryptonite", "bioluminescence", "plasma", nullptr
        };
        Enumeration_knob(f, &_presetCreative, kCreative,
                         "preset_creative", "creative");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Fictional / artistic materials combining emission, fluorescence,\\n"
                   "and volume absorption.");
    }
    {
        static const char* const kMetalRgl[] = {
            "(none)", "chrome steel", "brushed nickel", "brass", "tungsten",
            "anodized blue", nullptr
        };
        Enumeration_knob(f, &_presetMetalRgl, kMetalRgl,
                         "preset_metal_rgl", "metals (measured)");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "RGL approximations of measured metal BRDFs.\\n"
                   "Chrome, nickel, brass, tungsten, anodized. Uses tuned\\n"
                   "IOR and roughness rather than complex-IOR tables.");
    }
    {
        static const char* const kFabric[] = {
            "(none)", "silk", "velvet", "satin", "denim", "leather", nullptr
        };
        Enumeration_knob(f, &_presetFabric, kFabric,
                         "preset_fabric", "fabrics");
        Tooltip(f, "Cloth / soft goods approximations. Silk / satin / leather\\n"
                   "use clearcoat for subtle sheen.");
    }
    {
        static const char* const kCoating[] = {
            "(none)", "car paint red", "car paint black", "pearl white",
            "porcelain", nullptr
        };
        Enumeration_knob(f, &_presetCoating, kCoating,
                         "preset_coating", "coatings");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Layered coatings. Car paints and pearl use strong clearcoat.\\n"
                   "Porcelain uses subtle SSS for depth.");
    }
"""

# Append the closing tooltip block -- we dropped the original one above
# because the whole original tooltip section was inside CPP_KNOBS_OLD.
# The original continued for some lines; we need to find what's after
# the tooltip text to anchor the close.

CPP_TOOLTIP_TAIL_OLD = """               "  Metals, fabrics, car paints, porcelain.\\n\\n"
               "Select 'custom' to set all values manually.");
    Text_knob(f,
        "<font color='#888' size='-1'>"
        "Tip: presets reset all properties. Tweak after selecting."
        "</font>"
    );
"""

CPP_TOOLTIP_TAIL_NEW = """"""


# ============================================================================
#  CPP: knob_changed dispatch for the new category enums
# ============================================================================

CPP_HANDLER_OLD = """int SpectralSurfaceOp::knob_changed(Knob* k)
{
    // The preset enum fires knob_changed with k='preset' the FIRST time
    // the user picks a preset, but subsequent selections update the
    // bound _spectralPreset member WITHOUT firing knob_changed on the
    // preset knob. Diag traces confirm this: second pick shows
    // _spectralPreset=13 on the NEXT knob_changed for an unrelated
    // knob, but no intervening k='preset' event ever arrives.
    //
    // So rather than relying on k->is("preset"), check on every
    // knob_changed whether _spectralPreset has drifted from the last
    // value we applied. If so, apply it now. First interaction after
    // a preset pick (usually the user's own slider tweak, or even a
    // panel redraw) catches it up. Combined with the initial direct
    // fire for the very first pick, every preset change gets applied.
    if (_spectralPreset != _lastAppliedPreset) {
        _ApplyPreset(_spectralPreset);
        _lastAppliedPreset = _spectralPreset;
        // Force Nuke to refresh the panel now. Without this, the bound
        // member variables update but the sliders keep showing their
        // old positions because set_value calls inside _ApplyPreset get
        // queued/suppressed when we're already nested inside some other
        // knob's knob_changed callback.
        asapUpdate();
    }

    // Always update registry + bump global version so SpectralRender detects changes
    RegisterParams();
    s_spectralSurfaceVersion.fetch_add(1);
    return ShaderOp::knob_changed(k);
}
"""

CPP_HANDLER_NEW = """int SpectralSurfaceOp::knob_changed(Knob* k)
{
    // Legacy single-preset enum -- drift-detection path kept for
    // backward compatibility. The new per-category enums below
    // dispatch explicitly via k->is().
    if (_spectralPreset != _lastAppliedPreset) {
        _ApplyPreset(_spectralPreset);
        _lastAppliedPreset = _spectralPreset;
        asapUpdate();
    }

    // New per-category preset enums. Each dispatches _ApplyPresetV2
    // with a category tag and the preset index within that category.
    // Picking "(none)" (index 0) does nothing, so a fresh graph with
    // all enums at 0 preserves the SpectralMaterial defaults.
    if (k->is("preset_dielectric") && _presetDielectric > 0) {
        _ApplyPresetV2(0, _presetDielectric);
    }
    else if (k->is("preset_metal") && _presetMetal > 0) {
        _ApplyPresetV2(1, _presetMetal);
    }
    else if (k->is("preset_organic") && _presetOrganic > 0) {
        _ApplyPresetV2(2, _presetOrganic);
    }
    else if (k->is("preset_spectral") && _presetSpectral > 0) {
        _ApplyPresetV2(3, _presetSpectral);
    }
    else if (k->is("preset_creative") && _presetCreative > 0) {
        _ApplyPresetV2(4, _presetCreative);
    }
    else if (k->is("preset_metal_rgl") && _presetMetalRgl > 0) {
        _ApplyPresetV2(5, _presetMetalRgl);
    }
    else if (k->is("preset_fabric") && _presetFabric > 0) {
        _ApplyPresetV2(6, _presetFabric);
    }
    else if (k->is("preset_coating") && _presetCoating > 0) {
        _ApplyPresetV2(7, _presetCoating);
    }

    // Always update registry + bump global version so SpectralRender detects changes
    RegisterParams();
    s_spectralSurfaceVersion.fetch_add(1);
    return ShaderOp::knob_changed(k);
}
"""


# ============================================================================
#  CPP: the new _ApplyPresetV2 implementation, appended after _ApplyPreset
# ============================================================================

CPP_APPLY_V2_OLD = """void SpectralSurfaceOp::_SetShaderProperties(usg::ShaderDesc& desc,
                                              const MaterialContext& rtx)
"""

CPP_APPLY_V2_NEW = """// ---------------------------------------------------------------------------
// _ApplyPresetV2 -- new preset application path for the per-category UI.
//
// Key difference from _ApplyPreset: every material value is written via
// Knob::set_value() ONLY, never by direct member assignment. This is
// what makes the panel actually refresh -- Nuke's change detection
// compares the old bound value (pre-set_value) against the new value
// passed to set_value. If we'd written the member first, the bound
// variable would already equal the new value and Nuke would skip the
// UI redraw. That was the root cause of the preset-UI-refresh bug.
//
// category: 0=dielectric 1=metal 2=organic 3=spectral 4=creative
//           5=metal-rgl 6=fabric 7=coating
// preset:   1-based index within that category (0="(none)" is handled
//           by the caller, we assume > 0 here).
// ---------------------------------------------------------------------------
void SpectralSurfaceOp::_ApplyPresetV2(int category, int preset)
{
    // Local temporaries for every knob. Populated from the preset
    // table below, then flushed to knobs via set_value at the end.
    // Defaults match a "fresh custom material" (no advanced features).
    float diffuse[3]  = { 0.8f, 0.8f, 0.8f };
    float metallic    = 0.f;
    float roughness   = 0.5f;
    float ior         = 1.5f;
    float opacity     = 1.f;
    float emissive[3] = { 0.f, 0.f, 0.f };
    float clearcoat   = 0.f;
    float clearRough  = 0.f;
    float abbe        = 0.f;
    float thinFilm    = 0.f;
    int   metalType   = 0;
    float absColor[3] = { 1.f, 1.f, 1.f };
    float absDensity  = 0.f;
    float gratingSp   = 0.f;
    float gratingStr  = 1.f;
    float flAbsorb    = 0.f;
    float flEmit      = 0.f;
    float flStrength  = 0.f;
    float sssCol[3]   = { 0.f, 0.f, 0.f };
    float sssRad      = 0.f;

    // Dispatch table. Mirrors the old _ApplyPreset but populates
    // locals instead of members. Nested switch keeps the category
    // scope explicit and makes mistakes harder.
    switch (category) {
    case 0: // dielectric
        switch (preset) {
        case 1: // glass
            diffuse[0]=diffuse[1]=diffuse[2]=0.95f;
            metallic=0.f; roughness=0.f; ior=1.52f;
            opacity=0.002f; abbe=58.f;
            break;
        case 2: // diamond
            diffuse[0]=diffuse[1]=diffuse[2]=0.97f;
            metallic=0.f; roughness=0.f; ior=2.42f;
            opacity=0.002f; abbe=55.f;
            break;
        case 3: // water
            diffuse[0]=0.95f; diffuse[1]=0.95f; diffuse[2]=0.98f;
            metallic=0.f; roughness=0.f; ior=1.333f;
            opacity=0.15f; abbe=55.f;
            absColor[0]=0.4f; absColor[1]=0.75f; absColor[2]=0.9f;
            absDensity=1.f;
            break;
        case 4: // ruby
            diffuse[0]=0.8f; diffuse[1]=0.05f; diffuse[2]=0.1f;
            metallic=0.f; roughness=0.05f; ior=1.77f; opacity=0.3f;
            absColor[0]=0.9f; absColor[1]=0.05f; absColor[2]=0.1f;
            absDensity=3.f; abbe=45.f;
            flAbsorb=410.f; flEmit=694.f; flStrength=1.5f;
            break;
        }
        break;
    case 1: // metal
        switch (preset) {
        case 1: // copper
            diffuse[0]=0.95f; diffuse[1]=0.64f; diffuse[2]=0.54f;
            metallic=1.f; roughness=0.2f; ior=1.1f;
            metalType=2;
            break;
        case 2: // gold
            diffuse[0]=1.f; diffuse[1]=0.76f; diffuse[2]=0.33f;
            metallic=1.f; roughness=0.1f; ior=0.47f;
            metalType=1;
            break;
        case 3: // silver
            diffuse[0]=0.97f; diffuse[1]=0.96f; diffuse[2]=0.91f;
            metallic=1.f; roughness=0.05f; ior=0.18f;
            metalType=3;
            break;
        case 4: // aluminium
            diffuse[0]=0.91f; diffuse[1]=0.92f; diffuse[2]=0.92f;
            metallic=1.f; roughness=0.15f; ior=1.39f;
            metalType=4;
            break;
        }
        break;
    case 2: // organic
        switch (preset) {
        case 1: // skin
            diffuse[0]=0.76f; diffuse[1]=0.57f; diffuse[2]=0.45f;
            roughness=0.5f; ior=1.4f;
            sssCol[0]=0.9f; sssCol[1]=0.4f; sssCol[2]=0.2f; sssRad=0.5f;
            break;
        case 2: // wood
            diffuse[0]=0.43f; diffuse[1]=0.30f; diffuse[2]=0.18f;
            roughness=0.7f;
            break;
        case 3: // white paper
            diffuse[0]=0.75f; diffuse[1]=0.73f; diffuse[2]=0.70f;
            roughness=0.9f;
            break;
        case 4: // concrete
            diffuse[0]=0.55f; diffuse[1]=0.53f; diffuse[2]=0.50f;
            roughness=0.95f;
            break;
        case 5: // rubber
            diffuse[0]=0.05f; diffuse[1]=0.05f; diffuse[2]=0.05f;
            roughness=0.85f;
            break;
        case 6: // jade
            diffuse[0]=0.15f; diffuse[1]=0.5f; diffuse[2]=0.2f;
            roughness=0.3f; ior=1.66f;
            sssCol[0]=0.2f; sssCol[1]=0.7f; sssCol[2]=0.25f; sssRad=0.3f;
            break;
        }
        break;
    case 3: // spectral
        switch (preset) {
        case 1: // CD/DVD
            diffuse[0]=0.1f; diffuse[1]=0.1f; diffuse[2]=0.12f;
            metallic=0.8f; roughness=0.05f;
            gratingSp=0.46f; gratingStr=0.25f;
            break;
        case 2: // soap bubble
            diffuse[0]=0.95f; diffuse[1]=0.95f; diffuse[2]=0.98f;
            roughness=0.f; ior=1.33f; opacity=0.05f;
            thinFilm=350.f;
            break;
        case 3: // highlighter
            diffuse[0]=0.8f; diffuse[1]=1.f; diffuse[2]=0.1f;
            roughness=0.8f;
            flAbsorb=350.f; flEmit=520.f; flStrength=3.f;
            break;
        }
        break;
    case 4: // creative
        switch (preset) {
        case 1: // kryptonite
            diffuse[0]=0.1f; diffuse[1]=0.9f; diffuse[2]=0.15f;
            roughness=0.3f; ior=1.8f; opacity=0.4f;
            flAbsorb=380.f; flEmit=540.f; flStrength=4.f;
            absColor[0]=0.2f; absColor[1]=0.95f; absColor[2]=0.3f;
            absDensity=1.5f;
            break;
        case 2: // bioluminescence
            diffuse[0]=0.05f; diffuse[1]=0.15f; diffuse[2]=0.2f;
            roughness=0.6f; ior=1.4f;
            emissive[1]=0.5f; emissive[2]=0.8f;
            flAbsorb=400.f; flEmit=480.f; flStrength=2.f;
            break;
        case 3: // plasma
            diffuse[0]=0.02f; diffuse[1]=0.02f; diffuse[2]=0.05f;
            roughness=0.f; ior=1.f; opacity=0.1f;
            emissive[0]=0.6f; emissive[1]=0.2f; emissive[2]=1.f;
            flAbsorb=350.f; flEmit=450.f; flStrength=3.f;
            break;
        }
        break;
    case 5: // metal rgl
        switch (preset) {
        case 1: // chrome steel
            diffuse[0]=0.55f; diffuse[1]=0.56f; diffuse[2]=0.56f;
            metallic=1.f; roughness=0.03f; ior=2.75f;
            break;
        case 2: // brushed nickel
            diffuse[0]=0.66f; diffuse[1]=0.64f; diffuse[2]=0.58f;
            metallic=1.f; roughness=0.25f; ior=1.85f;
            break;
        case 3: // brass
            diffuse[0]=0.89f; diffuse[1]=0.74f; diffuse[2]=0.42f;
            metallic=1.f; roughness=0.15f; ior=1.18f;
            break;
        case 4: // tungsten
            diffuse[0]=0.52f; diffuse[1]=0.50f; diffuse[2]=0.47f;
            metallic=1.f; roughness=0.1f; ior=3.5f;
            break;
        case 5: // anodized blue
            diffuse[0]=0.12f; diffuse[1]=0.25f; diffuse[2]=0.55f;
            metallic=0.7f; roughness=0.2f; ior=1.8f;
            metalType=4; thinFilm=250.f;
            break;
        }
        break;
    case 6: // fabric
        switch (preset) {
        case 1: // silk
            diffuse[0]=0.85f; diffuse[1]=0.78f; diffuse[2]=0.72f;
            roughness=0.35f; ior=1.55f;
            clearcoat=0.3f; clearRough=0.1f;
            break;
        case 2: // velvet
            diffuse[0]=0.25f; diffuse[1]=0.05f; diffuse[2]=0.08f;
            roughness=0.95f;
            break;
        case 3: // satin
            diffuse[0]=0.82f; diffuse[1]=0.80f; diffuse[2]=0.75f;
            roughness=0.45f;
            clearcoat=0.5f; clearRough=0.15f;
            break;
        case 4: // denim
            diffuse[0]=0.10f; diffuse[1]=0.15f; diffuse[2]=0.30f;
            roughness=0.85f;
            break;
        case 5: // leather
            diffuse[0]=0.35f; diffuse[1]=0.22f; diffuse[2]=0.12f;
            roughness=0.6f;
            clearcoat=0.15f; clearRough=0.3f;
            break;
        }
        break;
    case 7: // coating
        switch (preset) {
        case 1: // car paint red
            diffuse[0]=0.65f; diffuse[1]=0.04f; diffuse[2]=0.04f;
            metallic=0.3f; roughness=0.15f;
            clearcoat=1.f; clearRough=0.02f;
            break;
        case 2: // car paint black
            diffuse[0]=0.02f; diffuse[1]=0.02f; diffuse[2]=0.02f;
            metallic=0.1f; roughness=0.05f;
            clearcoat=1.f; clearRough=0.01f;
            break;
        case 3: // pearl white
            diffuse[0]=0.92f; diffuse[1]=0.90f; diffuse[2]=0.88f;
            metallic=0.15f; roughness=0.2f; ior=1.6f;
            clearcoat=0.8f; clearRough=0.05f;
            thinFilm=180.f;
            break;
        case 4: // porcelain
            diffuse[0]=0.93f; diffuse[1]=0.92f; diffuse[2]=0.89f;
            roughness=0.15f; ior=1.52f;
            sssCol[0]=0.95f; sssCol[1]=0.9f; sssCol[2]=0.85f; sssRad=0.1f;
            break;
        }
        break;
    }

    // Flush locals to knobs via set_value. This is the key to the UI
    // refresh fix -- members still hold the PREVIOUS value until
    // set_value runs, so Nuke's change detection fires correctly.
    Op* op = getOp();
    if (!op) return;
    auto setVec3 = [&](const char* name, const float v[3]) {
        if (Knob* k = op->knob(name)) {
            k->set_value(v[0], 0);
            k->set_value(v[1], 1);
            k->set_value(v[2], 2);
        }
    };
    auto setF = [&](const char* name, float v) {
        if (Knob* k = op->knob(name)) k->set_value(v);
    };
    setVec3("diffuse_color",     diffuse);
    setF("metallic",             metallic);
    setF("roughness",            roughness);
    setF("ior",                  ior);
    setF("opacity",              opacity);
    setVec3("emissive_color",    emissive);
    setF("clearcoat",            clearcoat);
    setF("clearcoat_roughness",  clearRough);
    setF("abbe_number",          abbe);
    setF("thin_film",            thinFilm);
    // _metalType has no knob -- it's a SpectralSurface-internal member
    // set only by presets. Direct write is fine here (no UI widget to
    // refresh).
    _metalType = metalType;
    setVec3("absorption_color",  absColor);
    setF("absorption_density",   absDensity);
    setF("grating_spacing",      gratingSp);
    setF("grating_strength",     gratingStr);
    setF("fluor_absorb",         flAbsorb);
    setF("fluor_emit",           flEmit);
    setF("fluor_strength",       flStrength);
    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
}

void SpectralSurfaceOp::_SetShaderProperties(usg::ShaderDesc& desc,
                                              const MaterialContext& rtx)
"""


EDITS_H   = [(
    "Header: category members + V2 method decl",
    H_OLD, H_NEW,
    "int _presetDielectric = 0;"
)]
EDITS_CPP = [
    (
        "Cpp: split preset enum into 9 categories",
        CPP_KNOBS_OLD, CPP_KNOBS_NEW,
        '"preset_dielectric", "dielectrics"'
    ),
    (
        "Cpp: knob_changed dispatch",
        CPP_HANDLER_OLD, CPP_HANDLER_NEW,
        'k->is("preset_dielectric")'
    ),
    (
        "Cpp: insert _ApplyPresetV2 function",
        CPP_APPLY_V2_OLD, CPP_APPLY_V2_NEW,
        "void SpectralSurfaceOp::_ApplyPresetV2(int category, int preset)"
    ),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
    # Explicit marker: a distinctive substring present in NEW but NOT
    # in OLD, unique in the file. If the marker is already in the
    # file, we've applied this edit before.
    if marker in text:
        return text, R.ALREADY
    c = text.count(o)
    if c == 0: return text, R.NOT_FOUND
    if c > 1:  return text, R.AMBIGUOUS
    return text.replace(o, n, 1), R.APPLIED


def process(path, edits, dry, force, bak):
    print(f"\n=== {path.name} ===")
    if not path.exists():
        print(f"  ERROR: missing: {path}"); return False
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text = original; ok = True
    for desc, a, b, marker in edits:
        text, s = apply_edit(text, a, b, marker)
        mk = {R.APPLIED:"[+]", R.ALREADY:"[=]", R.NOT_FOUND:"[!]", R.AMBIGUOUS:"[?]"}[s]
        print(f"  {mk} {desc}: {s}")
        if s in (R.NOT_FOUND, R.AMBIGUOUS): ok = False
    if text == original:
        print("  (no changes needed)"); return ok
    if not ok and not force:
        print("  SKIPPED WRITE"); return False
    if dry:
        print(f"  DRY RUN: {len(text)-len(original):+d} chars"); return ok
    ob = text.encode(); obk = original.encode()
    if uses_crlf:
        ob = ob.replace(b"\n", b"\r\n"); obk = obk.replace(b"\n", b"\r\n")
    bakp = path.with_suffix(path.suffix + bak)
    bakp.write_bytes(obk); path.write_bytes(ob)
    print(f"  wrote {path.name}; backup {bakp.name}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)
    bak = ".bak_presetsplit"
    ok = True
    ok &= process(args.src / "SpectralSurfaceOp.h",   EDITS_H,   args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralSurfaceOp.cpp", EDITS_CPP, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nDone. Rebuild. New preset UI: 8 category dropdowns in 3 rows.")
    print("Picking a preset should update all sliders immediately.")


if __name__ == "__main__":
    main()
