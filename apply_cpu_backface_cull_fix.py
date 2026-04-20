#!/usr/bin/env python3
"""
apply_cpu_backface_cull_fix.py -- make single-sided backface hits on
CPU invalidate the hit entirely, so alpha goes to zero, matching GPU.

Previous state: single-sided backface hits returned 0 radiance from
_ShadeSpectral but the outer loop still wrote alpha from material
opacity. Result: card back + doubleSided=false on CPU shows RGB=0
but alpha=solid. GPU: both zero (hit culled at intersection stage).

Fix: at the top of the hit-processing block in RenderFrame, when the
hit's triangle faceNormal . rayDir > 0 AND the material is single-
sided, set hit.tri = nullptr. The rest of the loop uses hit.valid()
checks that now correctly cascade into the "no hit" path.

This aligns CPU with GPU at the hit-acceptance level and resolves
the alpha mismatch without changing _ShadeSpectral's signature or
threading an out-parameter through multiple layers.

Note: only addresses primary-ray hits. Shadow rays already work
correctly (they go through their own loop, not _ShadeSpectral).
Bounce rays also use _ShadeSpectral recursively but the backface
check inside _ShadeSpectral at line 1809 already handles those
(returns 0 radiance, which is correct for secondary).
"""

import argparse
import sys
from pathlib import Path


# Anchor: the primary hit-processing entry in RenderFrame
HIT_OLD = """                            if (hit.valid()) {
                                const SpectralMaterial& mat = scene.GetMaterial(hit.tri->materialId);
                                GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                                GfVec3f hitPos = GfVec3f(worldHit);
                                GfVec3f rayDir = GfVec3f(ray.GetDirection());
                                unsigned int bounceSeed = seed + 100u;
                                int shadeBounces = std::max(maxBounces, camera.refractionBounces);
"""

HIT_NEW = """                            if (hit.valid()) {
                                const SpectralMaterial& mat = scene.GetMaterial(hit.tri->materialId);
                                GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                                GfVec3f hitPos = GfVec3f(worldHit);
                                GfVec3f rayDir = GfVec3f(ray.GetDirection());

                                // Single-sided backface cull: reject the hit entirely
                                // (like GPU does at intersection). Invalidating hit.tri
                                // propagates through all subsequent hit.valid() checks:
                                // radiance = 0, alpha = 0, no depth/objId write.
                                // Without this, CPU keeps solid alpha on backface hits
                                // even with doubleSided=false, diverging from GPU.
                                {
                                    const GfVec3f& fN = hit.tri->faceNormal;
                                    float RdotFaceN = rayDir[0]*fN[0] + rayDir[1]*fN[1] + rayDir[2]*fN[2];
                                    if (!mat.doubleSided && RdotFaceN > 0.f) {
                                        hit.tri = nullptr;
                                    }
                                }
                            }
                            if (hit.valid()) {
                                const SpectralMaterial& mat = scene.GetMaterial(hit.tri->materialId);
                                GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                                GfVec3f hitPos = GfVec3f(worldHit);
                                GfVec3f rayDir = GfVec3f(ray.GetDirection());
                                unsigned int bounceSeed = seed + 100u;
                                int shadeBounces = std::max(maxBounces, camera.refractionBounces);
"""


EDITS = [
    ("Primary hit: invalidate single-sided backface hits before shading",
     HIT_OLD, HIT_NEW,
     "Single-sided backface cull: reject the hit entirely"),
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
    bak = ".bak_bfalpha"
    ok = process(args.src / "SpectralIntegrator.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Test:")
    print("  - Card from back, doubleSided=false, CPU: expect RGB=0 + alpha=0 (was alpha=solid)")
    print("  - Card from back, doubleSided=true, CPU: should still render as before")
    print("  - Card from front, doubleSided=false, CPU: should still render (front-face ok)")


if __name__ == "__main__":
    main()
