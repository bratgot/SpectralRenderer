#!/usr/bin/env python3
"""
apply_preset_ui_polish.py -- post-split polish on the preset UI.

After apply_preset_split.py the 8 category enums work but look cramped
and sit unwrapped below the title. This patch:

  1. Wraps all 8 enums in BeginClosedGroup(f, "preset_grp", "Preset").
     Collapsed by default so a user tweaking sliders isn't distracted
     by the preset block unless they want to change preset.
  2. Reorganises layout to 2-per-row across 4 rows. Pairs align cleanly
     because label widths match within a row.
  3. Adds a header Text_knob inside the group explaining "pick one
     category" and a footer note that picks auto-reset other categories.
  4. Beefs up per-category tooltips with meaty technical notes:
     render gotchas, which bits work on GPU vs CPU, and so on.
  5. Teaches _ApplyPresetV2 to reset OTHER category enums to (none)
     after applying a preset, so the panel shows only the active
     category. Stops the "five presets all selected at once" confusion
     that users see when clicking multiple categories in sequence.

Precondition: apply_preset_split.py has already been applied.

Files touched: src/SpectralSurfaceOp.cpp
Idempotent via marker, CRLF-safe, backs up to .bak_presetpolish.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  Edit 1: transform loose 8-enum block into grouped + polished layout
# ============================================================================

# OLD matches what apply_preset_split.py produced -- 8 loose enums in 3/3/2
# layout with short tooltips.

OLD_ENUMS = """    // Legacy single-preset enum, kept INVISIBLE so old .nk files load
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

# NEW: wrap in BeginClosedGroup, reorganise layout to 2-per-row, add
# header + footer Text_knobs, significantly expand tooltips.
#
# Layout (4 rows of 2):
#   Row 1: dielectrics | metals
#   Row 2: metals (measured) | organic
#   Row 3: spectral | creative
#   Row 4: fabrics | coatings
#
# ClearFlags(STARTLINE) on the second knob of each row.

NEW_ENUMS = """    // Legacy single-preset enum, kept INVISIBLE so old .nk files load
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


# ============================================================================
#  Edit 2: Teach _ApplyPresetV2 to reset the OTHER category enums.
#
#  Right at the end of _ApplyPresetV2, after all the set_value calls, we
#  loop through the per-category preset member knobs and set_value(0) on
#  every one EXCEPT the active category.
# ============================================================================

OLD_RESET = """    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
}

void SpectralSurfaceOp::_SetShaderProperties(usg::ShaderDesc& desc,
                                              const MaterialContext& rtx)
"""

NEW_RESET = """    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);

    // Reset the OTHER category preset enums to (none) = index 0 so the
    // UI reflects which preset is actually active. Without this, the
    // user can pick glass->gold->wood->porcelain and the panel ends up
    // showing all four still selected (even though only porcelain's
    // values are applied). The set_value(0) also drives the bound
    // members and invalidates the widgets correctly.
    //
    // The active category's knob is skipped because we don't want to
    // wipe out the user's selection we just applied.
    static const char* const kCatKnobs[8] = {
        "preset_dielectric",   // 0
        "preset_metal",        // 1
        "preset_organic",      // 2
        "preset_spectral",     // 3
        "preset_creative",     // 4
        "preset_metal_rgl",    // 5
        "preset_fabric",       // 6
        "preset_coating",      // 7
    };
    for (int c = 0; c < 8; ++c) {
        if (c == category) continue;
        if (Knob* k = op->knob(kCatKnobs[c])) {
            k->set_value(0.0);
        }
    }
}

void SpectralSurfaceOp::_SetShaderProperties(usg::ShaderDesc& desc,
                                              const MaterialContext& rtx)
"""


EDITS = [
    (
        "Wrap preset enums in BeginClosedGroup + layout + tooltips",
        OLD_ENUMS, NEW_ENUMS,
        'BeginClosedGroup(f, "preset_grp", "Preset");'
    ),
    (
        "Reset other category enums in _ApplyPresetV2",
        OLD_RESET, NEW_RESET,
        "static const char* const kCatKnobs[8] = {"
    ),
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


def process(path, dry, force, bak):
    print(f"\n=== {path.name} ===")
    if not path.exists():
        print(f"  ERROR: missing: {path}"); return False
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text = original; ok = True
    for desc, a, b, marker in EDITS:
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
    bak = ".bak_presetpolish"
    ok = process(args.src / "SpectralSurfaceOp.cpp", args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nDone. Rebuild. Preset group now collapsed by default.")
    print("Expand it to see 8 categories arranged 2-per-row with detailed")
    print("tooltips. Pick any preset -> other categories auto-reset to (none).")


if __name__ == "__main__":
    main()
