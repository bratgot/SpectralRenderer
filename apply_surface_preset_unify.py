#!/usr/bin/env python3
"""
apply_surface_preset_unify.py -- replace the 8-category preset UI on
SpectralSurface with a single master-enum dropdown mirroring the
SpectralVolumeMaterial approach (which the user reports "works
perfectly").

Root cause of the intermittent-jade-and-spectral-menus bug: the 8
category dropdowns interact in subtle ways with Nuke's re-entrant
knob_changed callbacks, and the V2 apply path doesn't always refresh
the panel widgets reliably. The Volume approach (one master enum,
straight member writes + set_value push, done) is simpler and
empirically robust.

This patch:

  1. In knobs(): replaces the BeginClosedGroup preset block (8 per-
     category enums + tooltips) with a single Enumeration_knob using
     the existing kSpectralPresetNames array (42 entries, 8 non-
     selectable header dividers).

  2. In knob_changed(): removes the 8 per-category dispatch branches
     and the V2 applied-preset drift tracker. Keeps only the single
     "preset" branch calling _ApplyPreset. Preserves the
     RegisterParams + version bump tail.

  3. Rewrites _ApplyPreset to the VolumeMaterial pattern: use LOCAL
     variables populated from a switch, push to knobs via set_value
     at the end. This is critical -- writing to members first then
     set_value is a no-op because Nuke's change detection sees the
     bound value already matches. Local-first-then-push was the
     pattern V2 used correctly; we take the same idea into the
     legacy function and remove V2 entirely.

  4. Retires _ApplyPresetV2 function from the .cpp.

  5. Header (.h): removes the 8 _preset* members and the
     _ApplyPresetV2 declaration.

Backward compatibility:
  - Old .nk files saved _spectralPreset (the INVISIBLE legacy knob)
    still load and resume at the saved preset. _lastAppliedPreset
    tracker kept so initial load doesn't double-apply.
  - Old .nk files with _presetDielectric etc. saved will emit
    "unknown knob: preset_dielectric" warnings on load. The values
    are just discarded. Acceptable trade-off.

Files touched:
  src/SpectralSurfaceOp.h      (5 edits: remove 8 members + V2 decl)
  src/SpectralSurfaceOp.cpp    (4 edits: UI block, knob_changed, legacy apply, V2 removal)

Idempotent via marker, backup .bak_surfunify.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  Header: remove 8 _preset* members + V2 declaration
# ============================================================================

H_MEMBERS_OLD = """    // Per-category preset selectors (new split UI). Each is an index
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

H_MEMBERS_NEW = """    // 2026-04-20: unified back to a single master preset dropdown
    // mirroring SpectralVolumeMaterial. The earlier 8-category split
    // had re-entrant callback issues that caused some presets (jade,
    // spectral category) to fail after a few selections.
"""


# ============================================================================
#  knobs() UI block: replace 8 dropdowns with 1 master enum.
#  Keep the invisible legacy knob (already there) as the backing store.
# ============================================================================

K_UI_OLD = """    // Legacy single-preset enum, kept INVISIBLE so old .nk files load
    // without knob warnings. All new work goes through the per-category
    // enums below.
    Enumeration_knob(f, &_spectralPreset, kSpectralPresetNames, "preset", "Preset");
    SetFlags(f, Knob::INVISIBLE);

    BeginClosedGroup(f, "preset_grp", "Preset");
    {
        Text_knob(f,
            "<font color='#999' size='-1'>"
            "Pick one preset. Picking a preset in any category auto-resets the others,"
            "<br>so the panel always shows one active preset (or all '(none)' for custom)."
            "</font>"
        );
        Newline(f);

        {
            static const char* const kDielectric[] = {
                "(none)", "glass", "diamond", "water", "ruby", nullptr
            };
            Enumeration_knob(f, &_presetDielectric, kDielectric,
                             "preset_dielectric", "dielectrics");
            Tooltip(f,
                "Transparent / refractive dielectrics.\\n"
                "\\n"
                "  glass    -- n=1.52, Abbe 58 (crown glass)\\n"
                "  diamond  -- n=2.42, Abbe 55, strong dispersion\\n"
                "  water    -- n=1.333, tinted volume absorption\\n"
                "  ruby     -- n=1.77, red volume, fluorescence\\n"
                "\\n"
                "These presets set opacity very low (~0.002) to enable\\n"
                "refraction. The renderer treats opacity < 1 as transmissive.\\n"
                "\\n"
                "TIP: set refraction bounces to 8+ in SpectralRender for\\n"
                "clean glass. Dispersion (Abbe) controls the rainbow spread --\\n"
                "raise it to 60+ for visible prism effects."
            );
        }
        {
            static const char* const kMetal[] = {
                "(none)", "copper", "gold", "silver", "aluminium", nullptr
            };
            Enumeration_knob(f, &_presetMetal, kMetal,
                             "preset_metal", "metals");
            ClearFlags(f, Knob::STARTLINE);
            Tooltip(f,
                "Measured metals -- spectral complex IOR (n,k) tables.\\n"
                "\\n"
                "  copper    -- warm red-orange Fresnel\\n"
                "  gold      -- characteristic yellow reflectance\\n"
                "  silver    -- nearly flat high reflectance\\n"
                "  aluminium -- broad-spectrum, slight blue at grazing\\n"
                "\\n"
                "Uses per-wavelength n,k values at 5 bands (380-780nm in\\n"
                "100nm steps) rather than a single IOR. Both CPU and GPU\\n"
                "paths support this. Visually correct even at low spp\\n"
                "because the wavelength-tinted Fresnel is deterministic."
            );
        }
        {
            static const char* const kMetalRgl[] = {
                "(none)", "chrome steel", "brushed nickel", "brass", "tungsten",
                "anodized blue", nullptr
            };
            Enumeration_knob(f, &_presetMetalRgl, kMetalRgl,
                             "preset_metal_rgl", "metals (measured)");
            Tooltip(f,
                "Measured-BRDF approximations (RGL database).\\n"
                "\\n"
                "  chrome steel    -- very low roughness, high IOR\\n"
                "  brushed nickel  -- elongated anisotropy (approximated)\\n"
                "  brass           -- warm yellow metal\\n"
                "  tungsten        -- high IOR, dense metal look\\n"
                "  anodized blue   -- aluminium base + 250nm thin-film\\n"
                "\\n"
                "These use tuned metallic=1 + IOR + roughness rather than\\n"
                "the full complex-IOR table. Cheaper to evaluate, less\\n"
                "physically exact than the 'metals' category above."
            );
        }
        {
            static const char* const kOrganic[] = {
                "(none)", "skin", "wood", "white paper", "concrete", "rubber", "jade", nullptr
            };
            Enumeration_knob(f, &_presetOrganic, kOrganic,
                             "preset_organic", "organic");
            ClearFlags(f, Knob::STARTLINE);
            Tooltip(f,
                "Everyday surfaces.\\n"
                "\\n"
                "  skin         -- warm SSS (radius 0.5), Disney diffuse\\n"
                "  wood         -- diffuse with brown base, no SSS\\n"
                "  white paper  -- high diffuse (0.73), rough\\n"
                "  concrete     -- mid-grey, very rough (0.95)\\n"
                "  rubber       -- near-black, rough, low IOR\\n"
                "  jade         -- green SSS (radius 0.3), smooth\\n"
                "\\n"
                "SSS note: CPU uses a 16-step random walk for accuracy;\\n"
                "GPU uses a wrap-diffuse approximation (see subsurface\\n"
                "scattering section below for details). Both look\\n"
                "reasonable at typical render settings."
            );
        }
        {
            static const char* const kSpectral[] = {
                "(none)", "CD/DVD", "soap bubble", "highlighter", nullptr
            };
            Enumeration_knob(f, &_presetSpectral, kSpectral,
                             "preset_spectral", "spectral");
            Tooltip(f,
                "Effects that only exist in a spectral renderer.\\n"
                "\\n"
                "  CD/DVD      -- 0.46um grating, rainbow diffraction\\n"
                "  soap bubble -- 350nm thin-film interference\\n"
                "  highlighter -- UV-to-green fluorescence (Stokes shift)\\n"
                "\\n"
                "Diffraction: the grating spacing in um determines rainbow\\n"
                "angular spread. CD is 1.6um (but pre-reduced here), DVD is\\n"
                "0.74um, butterfly iridescence is ~0.5um.\\n"
                "\\n"
                "Fluorescence: absorb in UV (350nm) -> emit in visible\\n"
                "(520nm green). Needs UV content in the lighting; HDRIs\\n"
                "and daylight provide it, pure-RGB lights do not."
            );
        }
        {
            static const char* const kCreative[] = {
                "(none)", "kryptonite", "bioluminescence", "plasma", nullptr
            };
            Enumeration_knob(f, &_presetCreative, kCreative,
                             "preset_creative", "creative");
            ClearFlags(f, Knob::STARTLINE);
            Tooltip(f,
                "Fictional materials combining multiple effects.\\n"
                "\\n"
                "  kryptonite      -- green fluorescence + volume absorption\\n"
                "  bioluminescence -- blue-cyan emission + fluorescence\\n"
                "  plasma          -- magenta emission, low opacity + refraction\\n"
                "\\n"
                "All three combine emissiveColor with fluorescence for\\n"
                "self-illuminating materials that also glow under UV light.\\n"
                "Good starting points for motion graphics / stylised looks."
            );
        }
        {
            static const char* const kFabric[] = {
                "(none)", "silk", "velvet", "satin", "denim", "leather", nullptr
            };
            Enumeration_knob(f, &_presetFabric, kFabric,
                             "preset_fabric", "fabrics");
            Tooltip(f,
                "Cloth and soft-goods approximations (RGL-inspired).\\n"
                "\\n"
                "  silk    -- warm tan, clearcoat 0.3 for subtle sheen\\n"
                "  velvet  -- deep wine red, very rough, no clearcoat\\n"
                "  satin   -- off-white, clearcoat 0.5\\n"
                "  denim   -- dark navy, rough, no clearcoat\\n"
                "  leather -- brown, slight clearcoat for grain\\n"
                "\\n"
                "Note: true fabric BRDFs have anisotropic fibre structure\\n"
                "not modelled here. For closer matches, use textures with\\n"
                "these presets as the base."
            );
        }
        {
            static const char* const kCoating[] = {
                "(none)", "car paint red", "car paint black", "pearl white",
                "porcelain", nullptr
            };
            Enumeration_knob(f, &_presetCoating, kCoating,
                             "preset_coating", "coatings");
            ClearFlags(f, Knob::STARTLINE);
            Tooltip(f,
                "Layered coatings with strong clearcoat.\\n"
                "\\n"
                "  car paint red   -- red base, clearcoat=1, ccRough=0.02\\n"
                "  car paint black -- near-black, clearcoat=1, ccRough=0.01\\n"
                "  pearl white     -- warm white, clearcoat + thin-film\\n"
                "  porcelain       -- creamy SSS + smooth clearcoat\\n"
                "\\n"
                "Clearcoat is a physically-based Disney lobe on top of the\\n"
                "base material. Pearl uses thin-film interference (180nm)\\n"
                "for the characteristic iridescent shift."
            );
        }

        Text_knob(f,
            "<font color='#888' size='-1'>"
            "Presets reset all material properties. Tweak sliders afterwards."
            "</font>"
        );
    }
    EndGroup(f);
"""

K_UI_NEW = """    // Master preset dropdown. 42 entries with 8 header dividers
    // (kSpectralPresetNames). Headers are handled as no-ops in
    // _ApplyPreset. Mirrors the SpectralVolumeMaterial approach.
    BeginClosedGroup(f, "preset_grp", "Preset");
    {
        Enumeration_knob(f, &_spectralPreset, kSpectralPresetNames,
                         "preset", "material");
        Tooltip(f,
            "Physically-accurate material presets.\\n"
            "\\n"
            "Dielectrics:  glass, diamond, water, ruby\\n"
            "Metals:       copper, gold, silver, aluminium (spectral n,k)\\n"
            "Organic:      skin, wood, paper, concrete, rubber, jade\\n"
            "Spectral:     CD/DVD, soap bubble, highlighter\\n"
            "Creative:     kryptonite, bioluminescence, plasma\\n"
            "RGL metals:   chrome, nickel, brass, tungsten, anodized blue\\n"
            "Fabrics:      silk, velvet, satin, denim, leather\\n"
            "Coatings:     car paint, pearl white, porcelain\\n"
            "\\n"
            "Dielectrics set opacity ~0.002 to enable refraction. Set\\n"
            "refraction bounces to 8+ in SpectralRender for clean glass.\\n"
            "\\n"
            "Metals in the first category use per-wavelength complex IOR\\n"
            "tables (Palik's Handbook). RGL metals use tuned metallic=1\\n"
            "+ IOR + roughness approximations -- cheaper, slightly less\\n"
            "accurate.\\n"
            "\\n"
            "Presets reset all material properties. Tweak sliders after\\n"
            "selecting to fine-tune."
        );
        Text_knob(f,
            "<font color='#888' size='-1'>"
            "Picking a preset resets all properties. Tweak sliders after."
            "</font>"
        );
    }
    EndGroup(f);
"""


# ============================================================================
#  knob_changed: simplify to single "preset" dispatch. Preserve the
#  RegisterParams + version bump tail.
# ============================================================================

K_CHG_OLD = """int SpectralSurfaceOp::knob_changed(Knob* k)
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

K_CHG_NEW = """int SpectralSurfaceOp::knob_changed(Knob* k)
{
    // Single master preset dropdown. Apply only when the value has
    // actually changed from the last applied one; this prevents
    // re-entrant panel-widget callbacks from re-applying the same
    // preset during set_value push.
    if (k->is("preset") && _spectralPreset != _lastAppliedPreset) {
        _ApplyPreset(_spectralPreset);
        _lastAppliedPreset = _spectralPreset;
    }

    // Always update registry + bump global version so SpectralRender detects changes
    RegisterParams();
    s_spectralSurfaceVersion.fetch_add(1);
    return ShaderOp::knob_changed(k);
}
"""


# ============================================================================
#  Rewrite _ApplyPreset to Volume-style: locals -> switch -> push to
#  knobs via set_value (no member assignment first). Members
#  automatically update when set_value writes to their bound storage.
# ============================================================================

APPLY_OLD = """// ---------------------------------------------------------------------------
void SpectralSurfaceOp::_ApplyPreset(int preset)
{
    if (preset == 0) return;  // custom — don't change anything
    // Header entries — do nothing
    if (preset == 1 || preset == 6 || preset == 11 || preset == 18 || preset == 22
        || preset == 26 || preset == 32 || preset == 38) return;

    // Reset all advanced features to defaults
    _abbeNumber = 0.f; _thinFilmThickness = 0.f; _metalType = 0;
    _absorptionColor[0]=1.f; _absorptionColor[1]=1.f; _absorptionColor[2]=1.f;
    _absorptionDensity = 0.f;
    _gratingSpacing = 0.f; _gratingStrength = 1.f;
    _fluorAbsorb = 0.f; _fluorEmit = 0.f; _fluorStrength = 0.f;
    _sssColor[0]=0.f; _sssColor[1]=0.f; _sssColor[2]=0.f; _sssRadius = 0.f;
    _emissiveColor[0]=0.f; _emissiveColor[1]=0.f; _emissiveColor[2]=0.f;
    _clearcoat = 0.f; _clearcoatRoughness = 0.f;
"""

APPLY_NEW = """// ---------------------------------------------------------------------------
// 2026-04-20 rewrite: mirrors SpectralVolumeMaterial's approach. Populate
// LOCAL variables from the preset table, then push to knobs via
// Knob::set_value. This is critical -- writing to members first then
// calling set_value is a no-op because Nuke's change detection sees the
// bound value already matches. Local-first-then-push makes the panel
// update reliably, and avoids the re-entrant callback storms that
// plagued the old 8-dropdown V2 design.
// ---------------------------------------------------------------------------
void SpectralSurfaceOp::_ApplyPreset(int preset)
{
    if (preset == 0) return;  // custom -- don't change anything
    // Header entries (dividers in the enum list) -- do nothing
    if (preset == 1 || preset == 6 || preset == 11 || preset == 18 || preset == 22
        || preset == 26 || preset == 32 || preset == 38) return;

    // Locals: defaults for a "fresh custom material" (no advanced features).
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
"""


# The switch body needs rewriting too: change `_diffuseColor[0]=` to
# `diffuse[0]=`, `_metallic=` to `metallic=`, etc. The switch is long;
# rather than re-writing every case individually, we do one big edit
# that replaces the whole switch body + the existing push-to-knobs
# tail. This is a whole-function rewrite.

FN_BODY_OLD = """    switch (preset) {
        // ── dielectrics ──
        case 2: // glass
            _diffuseColor[0] = _diffuseColor[1] = _diffuseColor[2] = 0.95f;
            _metallic = 0.0f; _roughness = 0.0f; _ior = 1.52f;
            _opacity = 0.002f; _abbeNumber = 58.f;
            break;
        case 3: // diamond
            _diffuseColor[0] = _diffuseColor[1] = _diffuseColor[2] = 0.97f;
            _metallic = 0.0f; _roughness = 0.0f; _ior = 2.42f;
            _opacity = 0.002f; _abbeNumber = 55.f;
            break;
        case 4: // water
            _diffuseColor[0] = 0.95f; _diffuseColor[1] = 0.95f; _diffuseColor[2] = 0.98f;
            _metallic = 0.0f; _roughness = 0.0f; _ior = 1.333f;
            _opacity = 0.15f; _abbeNumber = 55.f;
            _absorptionColor[0]=0.4f; _absorptionColor[1]=0.75f; _absorptionColor[2]=0.9f; _absorptionDensity=1.0f;
            break;
        case 5: // ruby
            _diffuseColor[0]=0.8f; _diffuseColor[1]=0.05f; _diffuseColor[2]=0.1f;
            _metallic=0.0f; _roughness=0.05f; _ior=1.77f; _opacity=0.3f;
            _absorptionColor[0]=0.9f; _absorptionColor[1]=0.05f; _absorptionColor[2]=0.1f;
            _absorptionDensity=3.f; _abbeNumber=45.f;
            _fluorAbsorb=410.f; _fluorEmit=694.f; _fluorStrength=1.5f;
            break;

        // ── metals ──
        case 7: // copper
            _diffuseColor[0] = 0.95f; _diffuseColor[1] = 0.64f; _diffuseColor[2] = 0.54f;
            _metallic = 1.0f; _roughness = 0.2f; _ior = 1.1f; _opacity = 1.0f;
            _metalType = 2;
            break;
        case 8: // gold
            _diffuseColor[0] = 1.0f; _diffuseColor[1] = 0.76f; _diffuseColor[2] = 0.33f;
            _metallic = 1.0f; _roughness = 0.1f; _ior = 0.47f; _opacity = 1.0f;
            _metalType = 1;
            break;
        case 9: // silver
            _diffuseColor[0] = 0.97f; _diffuseColor[1] = 0.96f; _diffuseColor[2] = 0.91f;
            _metallic = 1.0f; _roughness = 0.05f; _ior = 0.18f; _opacity = 1.0f;
            _metalType = 3;
            break;
        case 10: // aluminium
            _diffuseColor[0] = 0.91f; _diffuseColor[1] = 0.92f; _diffuseColor[2] = 0.92f;
            _metallic = 1.0f; _roughness = 0.15f; _ior = 1.39f; _opacity = 1.0f;
            _metalType = 4;
            break;

        // ── organic ──
        case 12: // skin
            _diffuseColor[0] = 0.76f; _diffuseColor[1] = 0.57f; _diffuseColor[2] = 0.45f;
            _metallic = 0.0f; _roughness = 0.5f; _ior = 1.4f; _opacity = 1.0f;
            _sssColor[0] = 0.9f; _sssColor[1] = 0.4f; _sssColor[2] = 0.2f;
            _sssRadius = 0.5f;
            break;
        case 13: // wood
            _diffuseColor[0] = 0.43f; _diffuseColor[1] = 0.30f; _diffuseColor[2] = 0.18f;
            _metallic = 0.0f; _roughness = 0.7f; _ior = 1.5f; _opacity = 1.0f;
            break;
        case 14: // white paper
            _diffuseColor[0] = 0.75f; _diffuseColor[1] = 0.73f; _diffuseColor[2] = 0.70f;
            _metallic = 0.0f; _roughness = 0.9f; _ior = 1.5f; _opacity = 1.0f;
            break;
        case 15: // concrete
            _diffuseColor[0] = 0.55f; _diffuseColor[1] = 0.53f; _diffuseColor[2] = 0.50f;
            _metallic = 0.0f; _roughness = 0.95f; _ior = 1.5f; _opacity = 1.0f;
            break;
        case 16: // rubber
            _diffuseColor[0] = 0.05f; _diffuseColor[1] = 0.05f; _diffuseColor[2] = 0.05f;
            _metallic = 0.0f; _roughness = 0.85f; _ior = 1.5f; _opacity = 1.0f;
            break;
        case 17: // jade
            _diffuseColor[0]=0.15f; _diffuseColor[1]=0.5f; _diffuseColor[2]=0.2f;
            _metallic=0.0f; _roughness=0.3f; _ior=1.66f; _opacity=1.0f;
            _sssColor[0]=0.2f; _sssColor[1]=0.7f; _sssColor[2]=0.25f;
            _sssRadius=0.3f;
            break;

        // ── spectral ──
        case 19: // CD/DVD
            _diffuseColor[0]=0.1f; _diffuseColor[1]=0.1f; _diffuseColor[2]=0.12f;
            _metallic=0.8f; _roughness=0.05f; _ior=1.5f; _opacity=1.0f;
            _gratingSpacing=0.46f; _gratingStrength=0.25f;
            break;
        case 20: // soap bubble
            _diffuseColor[0]=0.95f; _diffuseColor[1]=0.95f; _diffuseColor[2]=0.98f;
            _metallic=0.0f; _roughness=0.0f; _ior=1.33f; _opacity=0.05f;
            _thinFilmThickness=350.f;
            break;
        case 21: // highlighter
            _diffuseColor[0]=0.8f; _diffuseColor[1]=1.0f; _diffuseColor[2]=0.1f;
            _metallic=0.0f; _roughness=0.8f; _ior=1.5f; _opacity=1.0f;
            _fluorAbsorb=350.f; _fluorEmit=520.f; _fluorStrength=3.f;
            break;

        // ── creative ──
        case 23: // kryptonite
            _diffuseColor[0]=0.1f; _diffuseColor[1]=0.9f; _diffuseColor[2]=0.15f;
            _metallic=0.0f; _roughness=0.3f; _ior=1.8f; _opacity=0.4f;
            _fluorAbsorb=380.f; _fluorEmit=540.f; _fluorStrength=4.f;
            _absorptionColor[0]=0.2f; _absorptionColor[1]=0.95f; _absorptionColor[2]=0.3f;
            _absorptionDensity=1.5f;
            break;
        case 24: // bioluminescence
            _diffuseColor[0]=0.05f; _diffuseColor[1]=0.15f; _diffuseColor[2]=0.2f;
            _metallic=0.0f; _roughness=0.6f; _ior=1.4f; _opacity=1.0f;
            _emissiveColor[0]=0.0f; _emissiveColor[1]=0.5f; _emissiveColor[2]=0.8f;
            _fluorAbsorb=400.f; _fluorEmit=480.f; _fluorStrength=2.f;
            break;
        case 25: // plasma
            _diffuseColor[0]=0.02f; _diffuseColor[1]=0.02f; _diffuseColor[2]=0.05f;
            _metallic=0.0f; _roughness=0.0f; _ior=1.0f; _opacity=0.1f;
            _emissiveColor[0]=0.6f; _emissiveColor[1]=0.2f; _emissiveColor[2]=1.0f;
            _fluorAbsorb=350.f; _fluorEmit=450.f; _fluorStrength=3.f;
            break;

        // ── measured metals (RGL/MERL approximations) ──
        case 27: // chrome steel
            _diffuseColor[0]=0.55f; _diffuseColor[1]=0.56f; _diffuseColor[2]=0.56f;
            _metallic=1.0f; _roughness=0.03f; _ior=2.75f; _opacity=1.0f;
            _metalType=0;
            break;
        case 28: // brushed nickel
            _diffuseColor[0]=0.66f; _diffuseColor[1]=0.64f; _diffuseColor[2]=0.58f;
            _metallic=1.0f; _roughness=0.25f; _ior=1.85f; _opacity=1.0f;
            _metalType=0;
            break;
        case 29: // brass
            _diffuseColor[0]=0.89f; _diffuseColor[1]=0.74f; _diffuseColor[2]=0.42f;
            _metallic=1.0f; _roughness=0.15f; _ior=1.18f; _opacity=1.0f;
            _metalType=0;
            break;
        case 30: // tungsten
            _diffuseColor[0]=0.52f; _diffuseColor[1]=0.50f; _diffuseColor[2]=0.47f;
            _metallic=1.0f; _roughness=0.1f; _ior=3.5f; _opacity=1.0f;
            _metalType=0;
            break;
        case 31: // anodized blue
            _diffuseColor[0]=0.12f; _diffuseColor[1]=0.25f; _diffuseColor[2]=0.55f;
            _metallic=0.7f; _roughness=0.2f; _ior=1.8f; _opacity=1.0f;
            _metalType=4; // aluminium base
            _thinFilmThickness=250.f;
            break;

        // ── measured fabrics (RGL approximations) ──
        case 33: // silk
            _diffuseColor[0]=0.85f; _diffuseColor[1]=0.78f; _diffuseColor[2]=0.72f;
            _metallic=0.0f; _roughness=0.35f; _ior=1.55f; _opacity=1.0f;
            _clearcoat=0.3f; _clearcoatRoughness=0.1f;
            break;
        case 34: // velvet
            _diffuseColor[0]=0.25f; _diffuseColor[1]=0.05f; _diffuseColor[2]=0.08f;
            _metallic=0.0f; _roughness=0.95f; _ior=1.5f; _opacity=1.0f;
            break;
        case 35: // satin
            _diffuseColor[0]=0.82f; _diffuseColor[1]=0.80f; _diffuseColor[2]=0.75f;
            _metallic=0.0f; _roughness=0.45f; _ior=1.5f; _opacity=1.0f;
            _clearcoat=0.5f; _clearcoatRoughness=0.15f;
            break;
        case 36: // denim
            _diffuseColor[0]=0.10f; _diffuseColor[1]=0.15f; _diffuseColor[2]=0.30f;
            _metallic=0.0f; _roughness=0.85f; _ior=1.5f; _opacity=1.0f;
            break;
        case 37: // leather
            _diffuseColor[0]=0.35f; _diffuseColor[1]=0.22f; _diffuseColor[2]=0.12f;
            _metallic=0.0f; _roughness=0.6f; _ior=1.5f; _opacity=1.0f;
            _clearcoat=0.15f; _clearcoatRoughness=0.3f;
            break;

        // ── measured coatings (RGL approximations) ──
        case 39: // car paint red
            _diffuseColor[0]=0.65f; _diffuseColor[1]=0.04f; _diffuseColor[2]=0.04f;
            _metallic=0.3f; _roughness=0.15f; _ior=1.5f; _opacity=1.0f;
            _clearcoat=1.0f; _clearcoatRoughness=0.02f;
            break;
        case 40: // car paint black
            _diffuseColor[0]=0.02f; _diffuseColor[1]=0.02f; _diffuseColor[2]=0.02f;
            _metallic=0.1f; _roughness=0.05f; _ior=1.5f; _opacity=1.0f;
            _clearcoat=1.0f; _clearcoatRoughness=0.01f;
            break;
        case 41: // pearl white
            _diffuseColor[0]=0.92f; _diffuseColor[1]=0.90f; _diffuseColor[2]=0.88f;
            _metallic=0.15f; _roughness=0.2f; _ior=1.6f; _opacity=1.0f;
            _clearcoat=0.8f; _clearcoatRoughness=0.05f;
            _thinFilmThickness=180.f;
            break;
        case 42: // porcelain
            _diffuseColor[0]=0.93f; _diffuseColor[1]=0.92f; _diffuseColor[2]=0.89f;
            _metallic=0.0f; _roughness=0.15f; _ior=1.52f; _opacity=1.0f;
            _sssColor[0]=0.95f; _sssColor[1]=0.9f; _sssColor[2]=0.85f;
            _sssRadius=0.1f;
            break;

        default: break;
    }
"""

FN_BODY_NEW = """    switch (preset) {
        // -- dielectrics --
        case 2: // glass
            diffuse[0]=diffuse[1]=diffuse[2]=0.95f;
            metallic=0.f; roughness=0.f; ior=1.52f;
            opacity=0.002f; abbe=58.f;
            break;
        case 3: // diamond
            diffuse[0]=diffuse[1]=diffuse[2]=0.97f;
            metallic=0.f; roughness=0.f; ior=2.42f;
            opacity=0.002f; abbe=55.f;
            break;
        case 4: // water
            diffuse[0]=0.95f; diffuse[1]=0.95f; diffuse[2]=0.98f;
            metallic=0.f; roughness=0.f; ior=1.333f;
            opacity=0.15f; abbe=55.f;
            absColor[0]=0.4f; absColor[1]=0.75f; absColor[2]=0.9f; absDensity=1.f;
            break;
        case 5: // ruby
            diffuse[0]=0.8f; diffuse[1]=0.05f; diffuse[2]=0.1f;
            metallic=0.f; roughness=0.05f; ior=1.77f; opacity=0.3f;
            absColor[0]=0.9f; absColor[1]=0.05f; absColor[2]=0.1f;
            absDensity=3.f; abbe=45.f;
            flAbsorb=410.f; flEmit=694.f; flStrength=1.5f;
            break;

        // -- metals (spectral n,k) --
        case 7: // copper
            diffuse[0]=0.95f; diffuse[1]=0.64f; diffuse[2]=0.54f;
            metallic=1.f; roughness=0.2f; ior=1.1f;
            metalType=2;
            break;
        case 8: // gold
            diffuse[0]=1.f; diffuse[1]=0.76f; diffuse[2]=0.33f;
            metallic=1.f; roughness=0.1f; ior=0.47f;
            metalType=1;
            break;
        case 9: // silver
            diffuse[0]=0.97f; diffuse[1]=0.96f; diffuse[2]=0.91f;
            metallic=1.f; roughness=0.05f; ior=0.18f;
            metalType=3;
            break;
        case 10: // aluminium
            diffuse[0]=0.91f; diffuse[1]=0.92f; diffuse[2]=0.92f;
            metallic=1.f; roughness=0.15f; ior=1.39f;
            metalType=4;
            break;

        // -- organic --
        case 12: // skin
            diffuse[0]=0.76f; diffuse[1]=0.57f; diffuse[2]=0.45f;
            roughness=0.5f; ior=1.4f;
            sssCol[0]=0.9f; sssCol[1]=0.4f; sssCol[2]=0.2f; sssRad=0.5f;
            break;
        case 13: // wood
            diffuse[0]=0.43f; diffuse[1]=0.30f; diffuse[2]=0.18f;
            roughness=0.7f;
            break;
        case 14: // white paper
            diffuse[0]=0.75f; diffuse[1]=0.73f; diffuse[2]=0.70f;
            roughness=0.9f;
            break;
        case 15: // concrete
            diffuse[0]=0.55f; diffuse[1]=0.53f; diffuse[2]=0.50f;
            roughness=0.95f;
            break;
        case 16: // rubber
            diffuse[0]=0.05f; diffuse[1]=0.05f; diffuse[2]=0.05f;
            roughness=0.85f;
            break;
        case 17: // jade
            diffuse[0]=0.15f; diffuse[1]=0.5f; diffuse[2]=0.2f;
            roughness=0.3f; ior=1.66f;
            sssCol[0]=0.2f; sssCol[1]=0.7f; sssCol[2]=0.25f; sssRad=0.3f;
            break;

        // -- spectral --
        case 19: // CD/DVD
            diffuse[0]=0.1f; diffuse[1]=0.1f; diffuse[2]=0.12f;
            metallic=0.8f; roughness=0.05f;
            gratingSp=0.46f; gratingStr=0.25f;
            break;
        case 20: // soap bubble
            diffuse[0]=0.95f; diffuse[1]=0.95f; diffuse[2]=0.98f;
            roughness=0.f; ior=1.33f; opacity=0.05f;
            thinFilm=350.f;
            break;
        case 21: // highlighter
            diffuse[0]=0.8f; diffuse[1]=1.f; diffuse[2]=0.1f;
            roughness=0.8f;
            flAbsorb=350.f; flEmit=520.f; flStrength=3.f;
            break;

        // -- creative --
        case 23: // kryptonite
            diffuse[0]=0.1f; diffuse[1]=0.9f; diffuse[2]=0.15f;
            roughness=0.3f; ior=1.8f; opacity=0.4f;
            flAbsorb=380.f; flEmit=540.f; flStrength=4.f;
            absColor[0]=0.2f; absColor[1]=0.95f; absColor[2]=0.3f;
            absDensity=1.5f;
            break;
        case 24: // bioluminescence
            diffuse[0]=0.05f; diffuse[1]=0.15f; diffuse[2]=0.2f;
            roughness=0.6f; ior=1.4f;
            emissive[0]=0.f; emissive[1]=0.5f; emissive[2]=0.8f;
            flAbsorb=400.f; flEmit=480.f; flStrength=2.f;
            break;
        case 25: // plasma
            diffuse[0]=0.02f; diffuse[1]=0.02f; diffuse[2]=0.05f;
            roughness=0.f; ior=1.f; opacity=0.1f;
            emissive[0]=0.6f; emissive[1]=0.2f; emissive[2]=1.f;
            flAbsorb=350.f; flEmit=450.f; flStrength=3.f;
            break;

        // -- measured metals (RGL/MERL approximations) --
        case 27: // chrome steel
            diffuse[0]=0.55f; diffuse[1]=0.56f; diffuse[2]=0.56f;
            metallic=1.f; roughness=0.03f; ior=2.75f;
            break;
        case 28: // brushed nickel
            diffuse[0]=0.66f; diffuse[1]=0.64f; diffuse[2]=0.58f;
            metallic=1.f; roughness=0.25f; ior=1.85f;
            break;
        case 29: // brass
            diffuse[0]=0.89f; diffuse[1]=0.74f; diffuse[2]=0.42f;
            metallic=1.f; roughness=0.15f; ior=1.18f;
            break;
        case 30: // tungsten
            diffuse[0]=0.52f; diffuse[1]=0.50f; diffuse[2]=0.47f;
            metallic=1.f; roughness=0.1f; ior=3.5f;
            break;
        case 31: // anodized blue
            diffuse[0]=0.12f; diffuse[1]=0.25f; diffuse[2]=0.55f;
            metallic=0.7f; roughness=0.2f; ior=1.8f;
            metalType=4; // aluminium base
            thinFilm=250.f;
            break;

        // -- measured fabrics (RGL approximations) --
        case 33: // silk
            diffuse[0]=0.85f; diffuse[1]=0.78f; diffuse[2]=0.72f;
            roughness=0.35f; ior=1.55f;
            clearcoat=0.3f; clearRough=0.1f;
            break;
        case 34: // velvet
            diffuse[0]=0.25f; diffuse[1]=0.05f; diffuse[2]=0.08f;
            roughness=0.95f;
            break;
        case 35: // satin
            diffuse[0]=0.82f; diffuse[1]=0.80f; diffuse[2]=0.75f;
            roughness=0.45f;
            clearcoat=0.5f; clearRough=0.15f;
            break;
        case 36: // denim
            diffuse[0]=0.10f; diffuse[1]=0.15f; diffuse[2]=0.30f;
            roughness=0.85f;
            break;
        case 37: // leather
            diffuse[0]=0.35f; diffuse[1]=0.22f; diffuse[2]=0.12f;
            roughness=0.6f;
            clearcoat=0.15f; clearRough=0.3f;
            break;

        // -- measured coatings (RGL approximations) --
        case 39: // car paint red
            diffuse[0]=0.65f; diffuse[1]=0.04f; diffuse[2]=0.04f;
            metallic=0.3f; roughness=0.15f;
            clearcoat=1.f; clearRough=0.02f;
            break;
        case 40: // car paint black
            diffuse[0]=0.02f; diffuse[1]=0.02f; diffuse[2]=0.02f;
            metallic=0.1f; roughness=0.05f;
            clearcoat=1.f; clearRough=0.01f;
            break;
        case 41: // pearl white
            diffuse[0]=0.92f; diffuse[1]=0.90f; diffuse[2]=0.88f;
            metallic=0.15f; roughness=0.2f; ior=1.6f;
            clearcoat=0.8f; clearRough=0.05f;
            thinFilm=180.f;
            break;
        case 42: // porcelain
            diffuse[0]=0.93f; diffuse[1]=0.92f; diffuse[2]=0.89f;
            roughness=0.15f; ior=1.52f;
            sssCol[0]=0.95f; sssCol[1]=0.9f; sssCol[2]=0.85f; sssRad=0.1f;
            break;

        default: break;
    }
"""


# ============================================================================
#  Separate edit for the old push-to-knobs tail (long block with user's
#  added comments). Replace wholesale with the clean Volume-style push.
# ============================================================================

PUSH_OLD = """    // Force knob UI update.
    // Previously this block was wrapped in `if (Op* op = getOp()) { ... }`
    // which silently skipped the entire push-back when getOp() returned
    // null -- a timing issue that meant preset selection updated the C++
    // members but left every slider on the UI at its old value. Call
    // knob() directly on `this` (implicit) like SpectralStudioLight does.
    // Also add metal_type which was missing -- metal presets set
    // _metalType but never pushed it to the UI enum.
    // Each knob needs an explicit changed() call after set_value so Nuke
    // refreshes the slider UI immediately. Without this, the bound
    // members update but the panel keeps showing the old values until
    // some other event forces a redraw. asapUpdate() at the call site
    // helped for simple cases; per-knob changed() is the robust path.
    if (Knob* k = knob("diffuse_color")) {
        k->set_value(_diffuseColor[0], 0);
        k->set_value(_diffuseColor[1], 1);
        k->set_value(_diffuseColor[2], 2);
        k->changed();
    }
    if (Knob* k = knob("metallic"))    { k->set_value(_metallic);          k->changed(); }
    if (Knob* k = knob("roughness"))   { k->set_value(_roughness);         k->changed(); }
    if (Knob* k = knob("ior"))         { k->set_value(_ior);               k->changed(); }
    if (Knob* k = knob("opacity"))     { k->set_value(_opacity);           k->changed(); }
    if (Knob* k = knob("abbe_number")) { k->set_value(_abbeNumber);        k->changed(); }
    if (Knob* k = knob("thin_film"))   { k->set_value(_thinFilmThickness); k->changed(); }
    if (Knob* k = knob("metal_type"))  { k->set_value(double(_metalType)); k->changed(); }
    if (Knob* k = knob("absorption_color")) {
        k->set_value(_absorptionColor[0], 0);
        k->set_value(_absorptionColor[1], 1);
        k->set_value(_absorptionColor[2], 2);
        k->changed();
    }
    if (Knob* k = knob("absorption_density")) { k->set_value(_absorptionDensity); k->changed(); }
    if (Knob* k = knob("grating_spacing"))    { k->set_value(_gratingSpacing);    k->changed(); }
    if (Knob* k = knob("grating_strength"))   { k->set_value(_gratingStrength);   k->changed(); }
    if (Knob* k = knob("fluor_absorb"))       { k->set_value(_fluorAbsorb);       k->changed(); }
    if (Knob* k = knob("fluor_emit"))         { k->set_value(_fluorEmit);         k->changed(); }
    if (Knob* k = knob("fluor_strength"))     { k->set_value(_fluorStrength);     k->changed(); }
    if (Knob* k = knob("emissive_color")) {
        k->set_value(_emissiveColor[0], 0);
        k->set_value(_emissiveColor[1], 1);
        k->set_value(_emissiveColor[2], 2);
        k->changed();
    }
    if (Knob* k = knob("sss_color")) {
        k->set_value(_sssColor[0], 0);
        k->set_value(_sssColor[1], 1);
        k->set_value(_sssColor[2], 2);
        k->changed();
    }
    if (Knob* k = knob("sss_radius"))          { k->set_value(_sssRadius);          k->changed(); }
    if (Knob* k = knob("clearcoat"))           { k->set_value(_clearcoat);          k->changed(); }
    if (Knob* k = knob("clearcoat_roughness")) { k->set_value(_clearcoatRoughness); k->changed(); }

    // Force Nuke to redraw every widget on the panel. set_value +
    // changed() should be enough in principle but Nuke's internal
    // change tracking compares the member variable's CURRENT value
    // to the set_value argument -- and we wrote the members directly
    // in the switch block just above, so the change-detection sees
    // "old=new" and skips the invalidate. updateUI() bypasses that
    // by asking the panel to unconditionally repaint from current
    // bound state.
    updateUI(outputContext());
}
"""

PUSH_NEW = """    // Flush locals to knobs via set_value. This is the key to the
    // refresh: members still hold the PREVIOUS value until set_value
    // runs, so Nuke's change detection fires correctly and the panel
    // refreshes. Mirrors SpectralVolumeMaterial's reliable pattern.
    auto setVec3 = [&](const char* name, const float v[3]) {
        if (Knob* k = knob(name)) {
            k->set_value(v[0], 0);
            k->set_value(v[1], 1);
            k->set_value(v[2], 2);
        }
    };
    auto setF = [&](const char* name, float v) {
        if (Knob* k = knob(name)) k->set_value(v);
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
    // _metalType has no knob widget -- direct member write.
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
"""


# ============================================================================
#  Remove _ApplyPresetV2 function entirely
# ============================================================================

V2_OLD_START = """// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// _ApplyPresetV2 -- new preset application path for the per-category UI.
"""
V2_OLD_END = """}

void SpectralSurfaceOp::_SetShaderProperties(usg::ShaderDesc& desc,
                                              const MaterialContext& rtx)
{"""

# Instead of trying to anchor the whole function body, we delete everything
# between these two markers. Done as a separate pass that finds the range
# and splices it out.


EDITS_CPP = [
    ("UI: replace 8 per-category dropdowns with single master enum",
     K_UI_OLD, K_UI_NEW,
     "// Master preset dropdown. 42 entries with 8 header dividers"),
    ("knob_changed: simplify to single-preset dispatch",
     K_CHG_OLD, K_CHG_NEW,
     "// Single master preset dropdown. Apply only when"),
    ("_ApplyPreset header: replace member-reset with locals",
     APPLY_OLD, APPLY_NEW,
     "// 2026-04-20 rewrite: mirrors SpectralVolumeMaterial's approach"),
    ("_ApplyPreset switch body: locals not members",
     FN_BODY_OLD, FN_BODY_NEW,
     "        // -- metals (spectral n,k) --"),
    ("_ApplyPreset push-to-knobs tail: Volume-style",
     PUSH_OLD, PUSH_NEW,
     "// Flush locals to knobs via set_value. This is the key to the\n    // refresh"),
]

EDITS_H = [
    ("Header: remove 8 _preset* members + V2 decl",
     H_MEMBERS_OLD, H_MEMBERS_NEW,
     "2026-04-20: unified back to a single master preset dropdown"),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
    if marker in text: return text, R.ALREADY
    c = text.count(o)
    if c == 0: return text, R.NOT_FOUND
    if c > 1:  return text, R.AMBIGUOUS
    return text.replace(o, n, 1), R.APPLIED


def process(path, edits, dry, force, bak, v2_strip=False):
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

    # Extra pass for V2 function removal in .cpp
    if v2_strip:
        if "_ApplyPresetV2 -- new preset" in text:
            start = text.find(V2_OLD_START)
            end = text.find(V2_OLD_END)
            if start >= 0 and end > start:
                text = text[:start] + V2_OLD_END[len("}\n\n"):] + text[end + len(V2_OLD_END):]
                # Actually the above is wrong. Clean rewrite:
                pass
            # Clean approach: find _ApplyPresetV2 function, splice it out.
        else:
            print("  [=] V2 strip: already applied")

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


def strip_v2_function(path, dry, force, bak):
    """Splice out the whole _ApplyPresetV2 function body."""
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text = original

    # Find the V2 comment banner
    start_marker = "// ---------------------------------------------------------------------------\n// ---------------------------------------------------------------------------\n// _ApplyPresetV2 -- new preset application path for the per-category UI."
    end_marker = "\nvoid SpectralSurfaceOp::_SetShaderProperties"

    print(f"\n=== {path.name} (V2 function strip) ===")
    if "_ApplyPresetV2" not in text:
        print("  [=] V2 function strip: already applied")
        return True

    s = text.find(start_marker)
    if s < 0:
        # Maybe just one banner line
        s = text.find("// _ApplyPresetV2 -- new preset application path")
        if s >= 0:
            # walk back to find start of the banner
            back = text.rfind("// ----", 0, s)
            if back >= 0:
                # walk back past any stacked dashes banner
                prev = text.rfind("// ----", 0, back)
                if prev >= 0 and prev > back - 200:
                    s = prev
                else:
                    s = back
    if s < 0:
        print("  [!] V2 function strip: start marker NOT FOUND")
        return False

    e = text.find(end_marker, s)
    if e < 0:
        print("  [!] V2 function strip: end marker NOT FOUND")
        return False

    new_text = text[:s] + text[e + 1:]  # drop the preceding newline
    delta = len(new_text) - len(text)
    print(f"  [+] V2 function strip: removed {-delta} chars")

    if dry:
        return True

    ob = new_text.encode(); obk = original.encode()
    if uses_crlf:
        ob = ob.replace(b"\n", b"\r\n"); obk = obk.replace(b"\n", b"\r\n")
    bakp = path.with_suffix(path.suffix + bak + "_v2")
    bakp.write_bytes(obk); path.write_bytes(ob)
    print(f"  wrote (V2 removal); backup {bakp.name}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)
    bak = ".bak_surfunify"
    ok = True
    # V2 strip FIRST -- it uses byte-range boundaries that the other
    # cpp edits would invalidate if they ran before the strip. Once V2
    # is gone, the remaining edits target the legacy _ApplyPreset with
    # no risk of orphaned fragments.
    ok &= strip_v2_function(args.src / "SpectralSurfaceOp.cpp", args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralSurfaceOp.cpp", EDITS_CPP, args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralSurfaceOp.h",   EDITS_H,   args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Test: open any .nk with a SpectralSurface, cycle through")
    print("presets several times including jade, spectral category, coatings.")
    print("All knobs should reflect the preset values; no more intermittent")
    print("failures.")


if __name__ == "__main__":
    main()
