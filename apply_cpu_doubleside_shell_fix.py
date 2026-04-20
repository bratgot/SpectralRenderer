#!/usr/bin/env python3
"""
apply_cpu_doubleside_shell_fix.py -- CPU shadow-ray shell passthrough
for doubleSided meshes.

Problem: even after the origin-offset fix, a shadow ray from inside a
doubleSided sphere still self-shadows. Not from the local wall (origin
offset fixed that), but from the FAR wall of the sphere. The shadow
ray traverses the interior and hits the opposite wall as a backface
from its own POV; we currently treat that as occlusion.

Confirmed by tracer:
  DSDIAG shadow-hit: blocker matId=0 t=172.097 inShadow=1 transmit=1.000 (NdotL=0.906)
  DSDIAG shadow-hit: blocker matId=0 t=125.844 inShadow=1 transmit=1.000 (NdotL=0.700)
  (t-values are ~diameter; NdotL vs original N was positive-outward, so
  shadow ray is exiting through the far shell wall)

Fix: in the shadow loop, when the blocker's material is doubleSided
AND the shadow ray is hitting that triangle from behind (dot(L,faceN) > 0),
treat it as a shell-exit passthrough -- step past and continue, same
semantics as the existing glass / noShadowCast paths.

This matches GPU behavior (GPU already renders the sphere interior
correctly) and aligns with how other renderers treat doubleSided shells.

Trade-off: internal self-shadowing of concave doubleSided geometry
(cave interior lit by internal light) is suppressed. That's an
accepted limitation across most path tracers.
"""

import argparse
import sys
from pathlib import Path


# Anchor: just after the blocker material fetch, before the castsShadows
# and glass checks. The shell-passthrough check goes first because it's
# the cheapest (no texture sampling) and applies regardless of material
# opacity or castShadows setting.

SHADOW_OLD = """                SpectralBVH::Hit shadowHit = bvh.Intersect(shadowRay, rayTime);
                if (!shadowHit.valid()) break;  // reached light

                const SpectralMaterial& sMat = scene.GetMaterial(shadowHit.tri->materialId);

                // castsShadows=false: step ray past this hit and continue.
                // Semantically identical to glass passthrough except no
                // transmittance attenuation -- mesh is shadow-transparent.
                if (s_noShadowCastMatIds &&
                    s_noShadowCastMatIds->count(shadowHit.tri->materialId) > 0) {
                    shadowOrigin = GfVec3f(GfVec3d(shadowOrigin) + shadowHit.t * GfVec3d(L)) + L * 0.02f;
                    continue;
                }
"""

SHADOW_NEW = """                SpectralBVH::Hit shadowHit = bvh.Intersect(shadowRay, rayTime);
                if (!shadowHit.valid()) break;  // reached light

                const SpectralMaterial& sMat = scene.GetMaterial(shadowHit.tri->materialId);

                // Shell-passthrough for doubleSided meshes: if the
                // shadow ray is hitting the backface of a doubleSided
                // blocker, treat as not-occluded. The shadow is simply
                // exiting the shell. Without this, camera-inside-a-
                // doubleSided-sphere shadows itself via the far wall.
                // Matches GPU behavior.
                {
                    const GfVec3f& fN = shadowHit.tri->faceNormal;
                    float LdotFaceN = L[0]*fN[0] + L[1]*fN[1] + L[2]*fN[2];
                    if (sMat.doubleSided && LdotFaceN > 0.f) {
                        shadowOrigin = GfVec3f(GfVec3d(shadowOrigin) + shadowHit.t * GfVec3d(L)) + L * 0.02f;
                        continue;
                    }
                }

                // castsShadows=false: step ray past this hit and continue.
                // Semantically identical to glass passthrough except no
                // transmittance attenuation -- mesh is shadow-transparent.
                if (s_noShadowCastMatIds &&
                    s_noShadowCastMatIds->count(shadowHit.tri->materialId) > 0) {
                    shadowOrigin = GfVec3f(GfVec3d(shadowOrigin) + shadowHit.t * GfVec3d(L)) + L * 0.02f;
                    continue;
                }
"""


EDITS = [
    ("Shadow loop: shell-passthrough for doubleSided backface hits",
     SHADOW_OLD, SHADOW_NEW,
     "Shell-passthrough for doubleSided meshes"),
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
    bak = ".bak_shellfix"
    ok = process(args.src / "SpectralIntegrator.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Re-test inside-sphere on CPU:")
    print("  - doubleSided=true: should now be lit (shell passthrough lets shadow rays escape)")
    print("  - doubleSided=false: remains black (backface cull blocks primary ray)")
    print("Also verify regression cases:")
    print("  - Card from front, doubleSided=true, object between card and light:")
    print("    should show the object's shadow on the card (objects still cast shadows)")
    print("  - Card casts shadow on ground: doubleSided card should still cast shadow")
    print("    (shadow ray hits the FRONT face of the card from the light side, LdotFaceN<0,")
    print("    shell-passthrough doesn't trigger, normal occlusion path runs)")


if __name__ == "__main__":
    main()
