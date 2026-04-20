#!/usr/bin/env python3
"""
apply_cpu_doubleside_fix.py -- fix CPU self-shadowing when camera is
inside a convex doubleSided mesh.

Root cause (confirmed by apply_cpu_doubleside_diag tracers): when a ray
hits a backface and we flip N to face the camera (doubleSided branch),
subsequent shadow-ray offsets using `hitPos + N * 0.01f` push INTO the
surface instead of out of it. Shadow ray exits through the local wall,
registers the exit hit as occlusion, marks the pixel fully shadowed.

Trace confirmation:
  DSDIAG shadow-hit: blocker matId=0 t=0.607 inShadow=1 ...
  (blocker is the same mesh, t~0.6 = wall thickness = self-shadow)

Fix: offset shadow origin along the sign of N * L rather than N alone,
so the offset goes to whichever side of the surface the shadow ray
wants to exit. Matches the existing compat-shadow idiom on line 508.

Applies to:
  - Line 1830: direct-light shadow origin
  - Line 1873: volume-light shadow origin (same issue)

NOT touching (parked for follow-up if needed):
  - Line 1356: AO ray origin (could suffer similar issue)
  - Line 2442: indirect-bounce ray origin
  - Line 2748: transmission ray origin
These may also need the fix but are less frequent + user hasn't
reported symptoms matching them. Fix and test one thing at a time.
"""

import argparse
import sys
from pathlib import Path


# Edit 1: direct-light shadow origin (line 1830)
SHADOW_DIRECT_OLD = """            bool inShadow = false;
            float shadowTransmit = 1.f;
            GfVec3f shadowOrigin = hitPos + N * 0.01f;
            // DSDIAG: log shadow setup for backface hits (cap at 20)
"""

SHADOW_DIRECT_NEW = """            bool inShadow = false;
            float shadowTransmit = 1.f;
            // Offset along the shadow-ray-facing side of the surface so
            // we don't self-occlude when doubleSided flipped N inward
            // (e.g. camera inside a sphere). Same idiom as line ~508.
            float _NdotL_off = N[0]*L[0] + N[1]*L[1] + N[2]*L[2];
            GfVec3f shadowOrigin = hitPos + N * (_NdotL_off >= 0.f ? 0.01f : -0.01f);
            // DSDIAG: log shadow setup for backface hits (cap at 20)
"""


# Edit 2: volume-light shadow origin (line 1873). The block has a
# distinct pattern: `if (!inShadow && numVolumes > 0 && shadowTransmit > 0.01f) {`
# preceding. Anchor includes enough context to be unique.
SHADOW_VOLUME_OLD = """            // March shadow ray through volumes (cloud/fog shadows on surfaces)
            if (!inShadow && numVolumes > 0 && shadowTransmit > 0.01f) {
                GfVec3f sOrig = hitPos + N * 0.01f;
"""

SHADOW_VOLUME_NEW = """            // March shadow ray through volumes (cloud/fog shadows on surfaces)
            if (!inShadow && numVolumes > 0 && shadowTransmit > 0.01f) {
                // Same self-shadow-avoidance as the surface shadow origin above.
                float _NdotL_vol = N[0]*L[0] + N[1]*L[1] + N[2]*L[2];
                GfVec3f sOrig = hitPos + N * (_NdotL_vol >= 0.f ? 0.01f : -0.01f);
"""


EDITS = [
    ("Direct-light shadow origin: offset toward L-facing side",
     SHADOW_DIRECT_OLD, SHADOW_DIRECT_NEW,
     "Offset along the shadow-ray-facing side of the surface"),
    ("Volume shadow origin: offset toward L-facing side",
     SHADOW_VOLUME_OLD, SHADOW_VOLUME_NEW,
     "Same self-shadow-avoidance as the surface shadow origin above."),
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
    bak = ".bak_dsfix"
    ok = process(args.src / "SpectralIntegrator.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Re-run the inside-sphere test on CPU:")
    print("  - doubleSided=true: expect lit sphere interior (was black before)")
    print("  - doubleSided=false: should remain black (backface cull)")
    print("The DSDIAG tracers are still in place; you can remove them with a")
    print("cleanup patch once you've confirmed the fix works.")


if __name__ == "__main__":
    main()
