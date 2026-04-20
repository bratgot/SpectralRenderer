#!/usr/bin/env python3
"""
apply_cpu_doubleside_diag.py -- add tracers to CPU _ShadeSpectral
backface/shadow-ray path to confirm the self-shadow hypothesis.

Hypothesis: inside-sphere + doubleSided=true goes black on CPU because:
  1. Ray hits backface (NdotV < 0)
  2. doubleSided -> flip N, so N is now inward (toward camera)
  3. shadowOrigin = hitPos + N*0.01 pushes INTO the sphere
  4. Shadow ray traverses inward, exits through the FAR wall of the
     sphere, registers that exit hit as occlusion
  5. Light reported as fully shadowed -> black pixel

Traces added:
  - On backface hit with doubleSided=true: log NdotV (pre-flip), hitPos
  - On first few shadow rays from a backface hit: log shadowOrigin,
    L direction, what the shadow ray hit (material id, distance),
    light distance, inShadow result

Limited to the first ~10 samples per process via a static counter so
stderr doesn't explode. On a 2048x1556 render that's plenty of data
to confirm or refute the hypothesis.
"""

import argparse
import sys
from pathlib import Path


# Anchor: the NdotV flip block + shadow loop start.
# We add one log inside the flip-is-backface branch and one inside
# the shadow loop (just after computing shadowOrigin) both gated by
# a static counter.

# Edit 1: mark backface hits + capture the original normal
FLIP_OLD = """    // Ensure N faces the camera (flip if back-facing). If the material
    // is single-sided, a back-facing hit is treated as a miss: the ray
    // passes through unimpeded. Matches the GPU kernel and USD's
    // UsdGeomMesh.doubleSided semantics.
    float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
    if (NdotV < 0.f) {
        if (!mat.doubleSided) return 0.f;
        N = -N;
    }
"""

FLIP_NEW = """    // Ensure N faces the camera (flip if back-facing). If the material
    // is single-sided, a back-facing hit is treated as a miss: the ray
    // passes through unimpeded. Matches the GPU kernel and USD's
    // UsdGeomMesh.doubleSided semantics.
    float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
    bool _dsDiag_backface = (NdotV < 0.f);  // for diag tracers below
    if (NdotV < 0.f) {
        if (!mat.doubleSided) return 0.f;
        N = -N;
        // DSDIAG: log backface hits (cap at 10 total)
        {
            static int _dsdbf = 0;
            if (_dsdbf < 10) {
                _dsdbf++;
                fprintf(stderr, "DSDIAG backface: matId=%d doubleSided=%d "
                        "NdotV-pre-flip=%.3f hitPos=(%.3f,%.3f,%.3f) "
                        "N-after-flip=(%.3f,%.3f,%.3f) V=(%.3f,%.3f,%.3f)\\n",
                        tri.materialId, (int)mat.doubleSided, NdotV,
                        hitPos[0], hitPos[1], hitPos[2],
                        N[0], N[1], N[2], V[0], V[1], V[2]);
            }
        }
    }
"""

# Edit 2: inside the shadow loop, log the shadow-ray results for
# backface-hit shadows only (gated by _dsDiag_backface + static counter)
SHADOW_OLD = """            bool inShadow = false;
            float shadowTransmit = 1.f;
            GfVec3f shadowOrigin = hitPos + N * 0.01f;
"""

SHADOW_NEW = """            bool inShadow = false;
            float shadowTransmit = 1.f;
            GfVec3f shadowOrigin = hitPos + N * 0.01f;
            // DSDIAG: log shadow setup for backface hits (cap at 20)
            if (_dsDiag_backface) {
                static int _dsdsh = 0;
                if (_dsdsh < 20) {
                    _dsdsh++;
                    float NdotL = N[0]*L[0] + N[1]*L[1] + N[2]*L[2];
                    fprintf(stderr, "DSDIAG shadow-setup: shadowOrigin=(%.3f,%.3f,%.3f) "
                            "L=(%.3f,%.3f,%.3f) NdotL=%.3f lightType=%d\\n",
                            shadowOrigin[0], shadowOrigin[1], shadowOrigin[2],
                            L[0], L[1], L[2], NdotL, (int)light.type);
                }
            }
"""

# Edit 3: at the end of the shadow loop, log the resolved inShadow for
# backface cases. Simplest hook is right after `for (int sb... break; }`
# closes -- log inShadow + shadowTransmit.
SHADOW_RESULT_OLD = """                // Opaque: check distance
                if (light.type == SpectralLight::Type::Distant ||
                    light.type == SpectralLight::Type::Dome) {
                    inShadow = true;
                } else {
                    float lightDist = (light.position - hitPos).GetLength();
                    inShadow = (shadowHit.t < lightDist);
                }
                break;
            }
"""

SHADOW_RESULT_NEW = """                // Opaque: check distance
                if (light.type == SpectralLight::Type::Distant ||
                    light.type == SpectralLight::Type::Dome) {
                    inShadow = true;
                } else {
                    float lightDist = (light.position - hitPos).GetLength();
                    inShadow = (shadowHit.t < lightDist);
                }
                // DSDIAG: what the shadow ray hit (backface cases only)
                if (_dsDiag_backface) {
                    static int _dsdhit = 0;
                    if (_dsdhit < 20) {
                        _dsdhit++;
                        float NdotL = N[0]*L[0] + N[1]*L[1] + N[2]*L[2];
                        fprintf(stderr, "DSDIAG shadow-hit: blocker matId=%d t=%.3f "
                                "inShadow=%d transmit=%.3f (NdotL=%.3f)\\n",
                                shadowHit.tri->materialId, shadowHit.t,
                                (int)inShadow, shadowTransmit, NdotL);
                    }
                }
                break;
            }
"""


EDITS = [
    ("Mark backface hits + log with doubleSided state",
     FLIP_OLD, FLIP_NEW, "DSDIAG backface:"),
    ("Log shadow-ray setup for backface hits",
     SHADOW_OLD, SHADOW_NEW, "DSDIAG shadow-setup:"),
    ("Log shadow-ray resolution for backface hits",
     SHADOW_RESULT_OLD, SHADOW_RESULT_NEW, "DSDIAG shadow-hit:"),
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
    bak = ".bak_dsdiag"
    ok = process(args.src / "SpectralIntegrator.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Set up scene: camera INSIDE a sphere with SpectralMeshProperties")
    print("+ doubleSided=true (checked). Render one frame on CPU.")
    print("Expect DSDIAG lines:")
    print("  DSDIAG backface: ...     (confirms backface hits hit this code)")
    print("  DSDIAG shadow-setup: ... (shows shadow origin and direction)")
    print("  DSDIAG shadow-hit: ...   (if present, confirms self-shadow)")
    print("Paste those lines back.")


if __name__ == "__main__":
    main()
