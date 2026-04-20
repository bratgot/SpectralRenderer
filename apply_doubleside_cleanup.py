#!/usr/bin/env python3
"""
apply_doubleside_cleanup.py -- post-fix cleanup for the CPU doubleSided
debugging session.

Does two things:

1. SpectralIntegrator.cpp: strip the DSDIAG tracers installed by
   apply_cpu_doubleside_diag. Three blocks removed:
     - The _dsDiag_backface local + backface-hit logger
     - The shadow-setup logger
     - The shadow-hit logger
   The fixes they helped us find (origin-offset, shell-passthrough,
   primary-hit backface cull) all stay in place.

2. ROADMAP.txt: replace the "doubleSided intermittent" entry with a
   RESOLVED entry describing what actually went wrong. The original
   entry blamed GPU (stamping races, material sharing, kernel shade
   paths). None of that was the cause. GPU was fine; CPU had two
   separate unrelated bugs (self-shadow when camera inside convex
   doubleSided mesh; alpha-solid on single-sided backface hits).
   Keep the resolution documented so future-us doesn't repeat the
   same GPU-side triage.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  Cleanup 1: _dsDiag_backface local + backface-hit logger
# ============================================================================

DIAG1_OLD = """    float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
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

DIAG1_NEW = """    float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
    if (NdotV < 0.f) {
        if (!mat.doubleSided) return 0.f;
        N = -N;
    }
"""


# ============================================================================
#  Cleanup 2: shadow-setup logger
# ============================================================================

DIAG2_OLD = """            GfVec3f shadowOrigin = hitPos + N * (_NdotL_off >= 0.f ? 0.01f : -0.01f);
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

DIAG2_NEW = """            GfVec3f shadowOrigin = hitPos + N * (_NdotL_off >= 0.f ? 0.01f : -0.01f);
"""


# ============================================================================
#  Cleanup 3: shadow-hit logger
# ============================================================================

DIAG3_OLD = """                // DSDIAG: what the shadow ray hit (backface cases only)
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
"""

DIAG3_NEW = """                break;
"""


# ============================================================================
#  ROADMAP rewrite
# ============================================================================

ROADMAP_OLD = """- **SpectralMeshProperties doubleSided intermittent** (new, 2026-04-20).
  After apply_ds_gpu_fix landed the GPUParams struct field + both upload
  sites + matCheck coverage, doubleSided works in some scenes and not
  others. When it works: backfaces of a single-sided mesh disappear when
  the camera is interior (or primary rays otherwise hit backfaces). When
  it doesn't: toggling the knob produces no visible change, often on the
  same scene where it did work moments earlier. Suspects in priority
  order:
  (1) Stamping races against validate. Our stamp writes via const_cast
      into scene._materials after the triangulation pass but within the
      same _validate. If GPU.cpp computes matCheck from an earlier
      snapshot (e.g. cached vector address) the XOR won't see the flag.
      Triage: extend the MPDiag matCheck logger with the material's
      memory address + tid to confirm single-validate ordering.
  (2) Material sharing hidden by the Iop's material-resolution step.
      Two meshes with the same SpectralSurface produce one shared
      material ID. If one mesh is doubleSided and the other isn't, the
      stamp's last-write-wins overwrites the previous mesh's intent.
      User may be observing whichever mesh's stamp ran last in the
      iteration order, which can flip between validates if the USD
      Traverse() order isn't stable.
  (3) Kernel normal-flip condition. The gate lives inside the primary
      shade block. If the back-face hit is being handled by a DIFFERENT
      code path (transmission / clearcoat / etc.) the gate never fires.
      Might manifest as "doubleSided works on matte surfaces, fails on
      glass" or similar.
  Next action: add a per-material doubleSided tracer at kernel shade
  entry (cheap: one fprintf from host-side comparison of expected-vs-
  what-was-uploaded). Confirm matCheck XOR is actually being computed
  over the flipped flag. Only after that can we nail which of the three
  suspects is live.
"""

# Replace with a RESOLVED note documenting the real cause + fixes.
ROADMAP_NEW = """- **[RESOLVED 2026-04-21] SpectralMeshProperties doubleSided on CPU**
  (was tracked as "doubleSided intermittent"). Original triage blamed
  GPU stamping races and kernel shade-path gaps. Actual cause: GPU was
  fine. CPU had two unrelated bugs.
  Truth table from inside-sphere + card-backside tests:
    inside sphere, doubleSided=true : GPU lit, CPU BLACK (bug)
    inside sphere, doubleSided=false: GPU black alpha=0, CPU black alpha=0 (ok)
    card backside, doubleSided=true : GPU lit, CPU lit (ok)
    card backside, doubleSided=false: GPU black alpha=0, CPU black ALPHA=SOLID (bug)
  Bug 1 (inside-sphere self-shadow): shadow ray from backface-flipped N
    offset = hitPos + N*0.01 pushed shadow origin INTO the sphere, then
    the shadow ray exited through the far shell wall and registered that
    exit as occlusion. Two-part fix:
      - Sign-flip origin offset: `hitPos + N * (NdotL >= 0 ? 0.01 : -0.01)`
        at lines 1830 (direct shadow) and 1873 (volume shadow). Matches the
        pre-existing idiom on line 508 of the compat shadow path.
      - Shell-passthrough in the shadow loop: if the blocker is
        doubleSided AND `L . faceNormal > 0` (shadow ray exiting the
        blocker's back), step past and continue. Matches GPU behavior.
        Trade-off: internal self-shadowing of concave doubleSided geometry
        is suppressed (a cave interior lit by an internal light won't
        shadow itself). Accepted.
  Bug 2 (CPU alpha on single-sided backface): _ShadeSpectral returned 0
    radiance via `if (!mat.doubleSided) return 0.f` but the outer RenderFrame
    loop still wrote alpha from material.opacity. Fix: at the top of the
    primary-hit-processing block, if `rayDir . faceNormal > 0` AND
    material is single-sided, set `hit.tri = nullptr`. The downstream
    hit.valid() guards then cascade: no radiance, no alpha, no depth, no
    objId write. Matches GPU exactly (GPU culls at intersection stage).
  Patches: apply_cpu_doubleside_diag (tracers), apply_cpu_doubleside_fix
    (origin offset), apply_cpu_doubleside_shell_fix (shell passthrough),
    apply_cpu_backface_cull_fix (primary-hit invalidation).
  Leftover possible-latent: 4 other instances of the `hitPos + N * 0.01f`
    pattern remain unmodified (line 1356 AO, 2442 indirect bounce, 2748
    transmission). They may self-occlude similarly when rays originate
    from backface-flipped surfaces. Parked pending user-visible symptoms.
"""


EDITS_INTEGRATOR = [
    ("Remove _dsDiag_backface local + backface-hit logger",
     DIAG1_OLD, DIAG1_NEW, None),  # REMOVE: use old-not-in-text idempotency
    ("Remove shadow-setup logger",
     DIAG2_OLD, DIAG2_NEW, None),
    ("Remove shadow-hit logger",
     DIAG3_OLD, DIAG3_NEW, None),
]

EDITS_ROADMAP = [
    ("Rewrite doubleSided entry as RESOLVED with real cause + fixes",
     ROADMAP_OLD, ROADMAP_NEW, "[RESOLVED 2026-04-21] SpectralMeshProperties doubleSided on CPU"),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
    # marker=None: idempotent via "old not in text" (REMOVE-style edits)
    if marker is None:
        if o not in text:
            return text, R.ALREADY
    else:
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
    ap.add_argument("--roadmap", type=Path, default=Path("ROADMAP.txt"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)
    if not args.roadmap.exists():
        print(f"ERROR: --roadmap not found: {args.roadmap}", file=sys.stderr); sys.exit(1)
    bak = ".bak_dscleanup"
    ok = True
    ok &= process(args.src / "SpectralIntegrator.cpp", EDITS_INTEGRATOR, args.dry_run, args.force, bak)
    ok &= process(args.roadmap, EDITS_ROADMAP, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. No behavioral change -- tracers only removed, fix code stays.")


if __name__ == "__main__":
    main()
