#!/usr/bin/env python3
"""
apply_gpu_matcheck_extend.py -- extend the GPU material checksum so
sssColor / sssRadius / clearcoat / clearcoatRoughness trigger re-upload
of the material buffer when changed.

Root cause of "GPU clearcoat doesn't update":
  SpectralGPU.cpp around line 410 computes a checksum of every
  SpectralMaterial. The cached buffer on the GPU is only re-uploaded
  when the checksum changes. The existing checksum hashes metallic,
  roughness, ior, opacity, baseColor, abbeNumber, thinFilmThickness,
  absorptionDensity, gratingSpacing, fluorStrength, metalType -- but
  NOT the fields we added in Phase 2 (sss) or Phase 3 (clearcoat).
  Result: slider changes for those fields don't invalidate the GPU
  material buffer, so the kernel keeps shading with the old (default)
  values. CPU is unaffected because it reads from SpectralMaterial
  directly each frame.

Fix: add the missing fields to the checksum.

Files touched: src/SpectralGPU.cpp
Idempotent, CRLF-safe, backs up to .bak_matcheck.
"""

import argparse
import sys
from pathlib import Path


OLD = """            p.f = mats[i].fluorStrength; matCheck ^= p.u * 50331653u;
            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
        }
"""

NEW = """            p.f = mats[i].fluorStrength; matCheck ^= p.u * 50331653u;
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


EDITS = [
    ("Extend matCheck: sss + clearcoat fields", OLD, NEW),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n):
    if n in text and o not in text: return text, R.ALREADY
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
    for desc, a, b in edits:
        text, s = apply_edit(text, a, b)
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
    bak = ".bak_matcheck"
    ok = process(args.src / "SpectralGPU.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nApplied. Close Nuke, rebuild. Test: change clearcoat or\n"
          "clearcoatRoughness on SpectralSurface -- GPU should re-render\n"
          "immediately. Should see 'SpectralGPU: re-uploaded N materials\n"
          "(changed)' in stderr when changes trigger an upload.")


if __name__ == "__main__":
    main()
