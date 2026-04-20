#!/usr/bin/env python3
"""
apply_gpu_matcheck_full.py -- comprehensive audit of the GPU material
checksum in SpectralGPU.cpp. Supersedes apply_gpu_matcheck_extend.py
(which only patched sss + clearcoat).

Root cause of a whole class of "GPU doesn't update when I change X":
  The GPU material buffer is re-uploaded only when a checksum over all
  SpectralMaterial fields changes. The existing checksum only covers 11
  of the 23 fields that actually flow through to the GPU. Changing any
  of the other 12 silently does nothing on GPU until Nuke is restarted
  or the user tickles one of the hashed fields.

Missing fields this patch adds to the checksum:
  - emissiveColor[3]      (emissive color)
  - baseColorTexId        (base color texture ID)
  - textureBlend          (texture blend factor)
  - bumpMapTexId          (bump map texture ID)
  - bumpStrength          (bump intensity)
  - absorptionColor[3]    (volume color)
  - gratingStrength       (diffraction blend)
  - fluorAbsorb           (fluorescence absorption wavelength)
  - fluorEmit             (fluorescence emission wavelength)
  - sssColor[3]           (subsurface scatter colour) [Phase 2 field]
  - sssRadius             (subsurface scatter radius) [Phase 2 field]
  - clearcoat             (clearcoat strength)       [Phase 3 field]
  - clearcoatRoughness    (clearcoat roughness)      [Phase 3 field]

Already hashed fields left alone:
  metallic, roughness, ior, opacity, baseColor[3], abbeNumber,
  thinFilmThickness, absorptionDensity, gratingSpacing, fluorStrength,
  metalType

Fields NOT hashed (intentionally):
  - isShadowCatcher: computed from scene state, not user-editable
    per material. Changes through a different mechanism.

Each field uses a distinct odd multiplier (to avoid xor collisions on
identical values across fields). Multipliers are large primes.

Safe to apply BEFORE or INSTEAD OF apply_gpu_matcheck_extend.py. If the
extend patch was already applied, this detects it and supersedes.

Files touched: src/SpectralGPU.cpp
Idempotent, CRLF-safe, backs up to .bak_matcheckfull.
"""

import argparse
import sys
from pathlib import Path


# Two OLD variants: one for pre-extend state, one for post-extend state
# (in case user already applied apply_gpu_matcheck_extend.py).

OLD_PRE_EXTEND = """            p.f = mats[i].fluorStrength; matCheck ^= p.u * 50331653u;
            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
        }
"""

OLD_POST_EXTEND = """            p.f = mats[i].fluorStrength; matCheck ^= p.u * 50331653u;
            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
            // Phase 2 (SSS) + Phase 3 (clearcoat) fields. Without these
            // in the checksum, slider changes on the SpectralSurface UI
            // update SpectralMaterial on the CPU side but never trigger
            // a GPU material re-upload, so the kernel keeps shading with
            // the stale buffer (clearcoat=0, sssRadius=0, ...).
            p.f = mats[i].sssColor[0]; matCheck ^= p.u * 100663319u;
            p.f = mats[i].sssColor[1]; matCheck ^= p.u * 201326611u;
            p.f = mats[i].sssColor[2]; matCheck ^= p.u * 402653189u;
            p.f = mats[i].sssRadius;   matCheck ^= p.u * 805306457u;
            p.f = mats[i].clearcoat;          matCheck ^= p.u * 1610612741u;
            p.f = mats[i].clearcoatRoughness; matCheck ^= p.u * 24593u;
        }
"""


NEW = """            p.f = mats[i].fluorStrength; matCheck ^= p.u * 50331653u;
            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
            // --- Comprehensive audit (all fields that flow to GPU) ---
            // Missing from the original checksum; changing any of these
            // knobs silently did nothing on GPU because the material
            // buffer was never re-uploaded. Each uses a distinct large
            // prime multiplier so field values can't collide with each
            // other under XOR.
            p.f = mats[i].emissiveColor[0]; matCheck ^= p.u * 5449u;
            p.f = mats[i].emissiveColor[1]; matCheck ^= p.u * 8369u;
            p.f = mats[i].emissiveColor[2]; matCheck ^= p.u * 11329u;
            matCheck ^= static_cast<unsigned int>(mats[i].baseColorTexId + 1) * 14561u;
            p.f = mats[i].textureBlend;     matCheck ^= p.u * 17401u;
            matCheck ^= static_cast<unsigned int>(mats[i].bumpMapTexId + 1) * 20149u;
            p.f = mats[i].bumpStrength;     matCheck ^= p.u * 23071u;
            p.f = mats[i].absorptionColor[0]; matCheck ^= p.u * 26153u;
            p.f = mats[i].absorptionColor[1]; matCheck ^= p.u * 29339u;
            p.f = mats[i].absorptionColor[2]; matCheck ^= p.u * 32687u;
            p.f = mats[i].gratingStrength;  matCheck ^= p.u * 35993u;
            p.f = mats[i].fluorAbsorb;      matCheck ^= p.u * 39239u;
            p.f = mats[i].fluorEmit;        matCheck ^= p.u * 42577u;
            // Phase 2 (SSS) and Phase 3 (clearcoat) fields.
            p.f = mats[i].sssColor[0];      matCheck ^= p.u * 100663319u;
            p.f = mats[i].sssColor[1];      matCheck ^= p.u * 201326611u;
            p.f = mats[i].sssColor[2];      matCheck ^= p.u * 402653189u;
            p.f = mats[i].sssRadius;        matCheck ^= p.u * 805306457u;
            p.f = mats[i].clearcoat;          matCheck ^= p.u * 1610612741u;
            p.f = mats[i].clearcoatRoughness; matCheck ^= p.u * 46051u;
        }
"""


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply(text):
    """Try both anchors, prefer post-extend (the newer state)."""
    # Already applied?
    if NEW in text:
        return text, R.ALREADY, None
    # Try post-extend anchor first
    if OLD_POST_EXTEND in text:
        c = text.count(OLD_POST_EXTEND)
        if c > 1: return text, R.AMBIGUOUS, "post-extend (multiple)"
        return text.replace(OLD_POST_EXTEND, NEW, 1), R.APPLIED, "post-extend"
    # Fall back to pre-extend anchor
    if OLD_PRE_EXTEND in text:
        c = text.count(OLD_PRE_EXTEND)
        if c > 1: return text, R.AMBIGUOUS, "pre-extend (multiple)"
        return text.replace(OLD_PRE_EXTEND, NEW, 1), R.APPLIED, "pre-extend"
    return text, R.NOT_FOUND, None


def process(path, dry, force):
    print(f"\n=== {path.name} ===")
    if not path.exists():
        print(f"  ERROR: missing: {path}"); return False
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text, s, variant = apply(original)
    mk = {R.APPLIED:"[+]", R.ALREADY:"[=]", R.NOT_FOUND:"[!]", R.AMBIGUOUS:"[?]"}[s]
    label = f"Audit matCheck: add all unhashed fields"
    if variant: label += f" ({variant} anchor)"
    print(f"  {mk} {label}: {s}")
    ok = s not in (R.NOT_FOUND, R.AMBIGUOUS)
    if text == original:
        print("  (no changes needed)"); return ok
    if not ok and not force:
        print("  SKIPPED WRITE"); return False
    if dry:
        print(f"  DRY RUN: {len(text)-len(original):+d} chars"); return ok
    ob = text.encode(); obk = original.encode()
    if uses_crlf:
        ob = ob.replace(b"\n", b"\r\n"); obk = obk.replace(b"\n", b"\r\n")
    bakp = path.with_suffix(path.suffix + ".bak_matcheckfull")
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
    ok = process(args.src / "SpectralGPU.cpp", args.dry_run, args.force)
    if not ok: sys.exit(1)
    print("\nApplied. Rebuild. All 23 material fields now invalidate the")
    print("GPU buffer on change. Test: tweak any previously-unhashed knob")
    print("(emissive, textureBlend, bumpStrength, absorptionColor,")
    print(" gratingStrength, fluorAbsorb, fluorEmit) on GPU -- should")
    print("see 'SpectralGPU: re-uploaded N materials (changed)' each time.")


if __name__ == "__main__":
    main()
