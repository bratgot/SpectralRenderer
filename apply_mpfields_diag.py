#!/usr/bin/env python3
"""
apply_mpfields_diag.py -- diagnostic tracers for doubleSided /
orientation / purpose. User reports toggling doubleSided has no effect
on render; needs visibility into where the flag goes missing.

Adds four tracers:
  1. Extends the existing "mesh props for ..." log to include
     doubleSided / orientation / purpose values.
  2. Logs whether my stamp block executes + how many materials get
     stamped per mesh.
  3. Logs the doubleSided value for each material when GPU checksum
     runs (shows whether the flag ever hits GPU.cpp).
  4. Logs first N primary-ray hits on GPU kernel with NdotV + the
     doubleSided decision path taken. Helps confirm whether the gate
     ever fires even when camera is interior to geometry.

Applies on top of apply_meshprops_fields.py. If those edits aren't in,
re-apply them first.

Files touched (4 edits, 3 files):
  SpectralRenderIop.cpp       extend mesh-props log + stamp log
  SpectralGPU.cpp             log doubleSided in checksum loop
  SpectralGPUKernel.cu        first-N-rays hit decision log

Idempotent via marker, CRLF-safe, backup .bak_mpdiag.
Strip with apply_mpfields_diag_strip.py once diagnosed.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  (1) Extend mesh-props log to show the 3 new fields
# ============================================================================

IOP_LOG_OLD = """                SLOG("SpectralRender: mesh props for '%s' via node '%s' "
                     "(level=%d scheme=%d flipN=%d normMode=%d visible=%d castsShadows=%d)\\n",
                     meshPath.c_str(), matchedNodeName.c_str(),
                     meshSubdivOverride, meshSchemeOverride,
                     (int)meshFlipNormals, meshNormalMode,
                     (int)meshPropsVisible, (int)meshPropsCastsShadows);
"""

IOP_LOG_NEW = """                SLOG("SpectralRender: mesh props for '%s' via node '%s' "
                     "(level=%d scheme=%d flipN=%d normMode=%d visible=%d castsShadows=%d "
                     "doubleSided=%d orientation=%d purpose=%d)\\n",
                     meshPath.c_str(), matchedNodeName.c_str(),
                     meshSubdivOverride, meshSchemeOverride,
                     (int)meshFlipNormals, meshNormalMode,
                     (int)meshPropsVisible, (int)meshPropsCastsShadows,
                     (int)meshPropsDoubleSided, meshPropsOrientation, meshPropsPurpose);
"""


# ============================================================================
#  (2) Log stamp block even when doubleSided is TRUE (for verification)
# ============================================================================

IOP_STAMP_OLD = """            // Stamp per-mesh doubleSided onto the materials this mesh
            // uses. Last-write-wins for shared materials; usually a non-
            // issue since each mesh typically gets its own material
            // variant after resolution. Harmless when doubleSided=true
            // (that's the default on SpectralMaterial too).
            if (meshPropsHasEntry && !meshPropsDoubleSided) {
                std::unordered_set<int> stampIds;
                for (const auto& tri : data.triangles) {
                    stampIds.insert(static_cast<int>(tri.materialId));
                }
                auto& mats = const_cast<std::vector<SpectralMaterial>&>(
                    _scene->GetMaterials());
                for (int mid : stampIds) {
                    if (mid >= 0 && mid < (int)mats.size()) {
                        mats[mid].doubleSided = false;
                    }
                }
                SLOG("SpectralRender: mesh '%s' stamped doubleSided=false onto %zu materials\\n",
                     data.id.GetString().c_str(), stampIds.size());
            }
"""

IOP_STAMP_NEW = """            // Stamp per-mesh doubleSided onto the materials this mesh
            // uses. Last-write-wins for shared materials; usually a non-
            // issue since each mesh typically gets its own material
            // variant after resolution. Harmless when doubleSided=true
            // (that's the default on SpectralMaterial too).
            fprintf(stderr, "MPDiag: STAMP entry for mesh '%s' hasEntry=%d doubleSided=%d\\n",
                data.id.GetString().c_str(),
                meshPropsHasEntry ? 1 : 0,
                meshPropsDoubleSided ? 1 : 0);
            if (meshPropsHasEntry && !meshPropsDoubleSided) {
                std::unordered_set<int> stampIds;
                for (const auto& tri : data.triangles) {
                    stampIds.insert(static_cast<int>(tri.materialId));
                }
                auto& mats = const_cast<std::vector<SpectralMaterial>&>(
                    _scene->GetMaterials());
                for (int mid : stampIds) {
                    if (mid >= 0 && mid < (int)mats.size()) {
                        mats[mid].doubleSided = false;
                        fprintf(stderr, "MPDiag:   stamped material[%d].doubleSided=false\\n", mid);
                    }
                }
                SLOG("SpectralRender: mesh '%s' stamped doubleSided=false onto %zu materials\\n",
                     data.id.GetString().c_str(), stampIds.size());
            }
"""


# ============================================================================
#  (3) Log doubleSided values in GPU material checksum loop
# ============================================================================

GPU_CHK_OLD = """            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
"""

GPU_CHK_NEW = """            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
            fprintf(stderr, "MPDiag: matCheck material[%zu] doubleSided=%d\\n",
                i, mats[i].doubleSided ? 1 : 0);
"""


# ============================================================================
#  (4) Log first N primary-ray hit decisions on GPU kernel
# ============================================================================

KERNEL_OLD = """                float3 V = normalize3(neg3(dir));
                if (dot3raw(N,V)<0.f) {
                    if (!mat.doubleSided) {
                        // Single-sided surface, back-facing hit ->
                        // treat as miss. Ray passes through.
                        isHit = false;
                        goto spectral_miss;
                    }
                    N=neg3(N);
                }
"""

KERNEL_NEW = """                float3 V = normalize3(neg3(dir));
                // MPDiag: log first handful of rays to confirm gate reachability
                {
                    int diagPixIdx = int(px)*int(H) + int(py);
                    if (diagPixIdx < 4) {
                        printf("MPDiagKernel: px=%d py=%d NdotV=%.3f matId=%d doubleSided=%d\\n",
                            (int)px, (int)py, dot3raw(N,V), matId, mat.doubleSided);
                    }
                }
                if (dot3raw(N,V)<0.f) {
                    if (!mat.doubleSided) {
                        // Single-sided surface, back-facing hit ->
                        // treat as miss. Ray passes through.
                        isHit = false;
                        goto spectral_miss;
                    }
                    N=neg3(N);
                }
"""


# ============================================================================
#  Apply scaffold
# ============================================================================

EDITS_IOP = [
    ("Iop: extend mesh-props log with doubleSided/orientation/purpose",
     IOP_LOG_OLD, IOP_LOG_NEW,
     'doubleSided=%d orientation=%d purpose=%d'),
    ("Iop: log stamp block entry + per-material stamp",
     IOP_STAMP_OLD, IOP_STAMP_NEW,
     "MPDiag: STAMP entry"),
]
EDITS_GPU = [
    ("GPU.cpp: log doubleSided in material checksum loop",
     GPU_CHK_OLD, GPU_CHK_NEW,
     "MPDiag: matCheck material"),
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
    bak = ".bak_mpdiag"
    ok = True
    ok &= process(args.src / "SpectralRenderIop.cpp", EDITS_IOP,    args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPU.cpp",       EDITS_GPU,    args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Toggle doubleSided on a SpectralMeshProperties node and")
    print("grab stderr. Look for 'MPDiag:' lines:")
    print("")
    print("  Flow check:")
    print("    1. 'mesh props for X' log now shows doubleSided value.")
    print("       If the value doesn't flip when you toggle the knob, the bug")
    print("       is upstream (registry isn't receiving the new value).")
    print("    2. 'MPDiag: STAMP entry' shows hasEntry + doubleSided per mesh.")
    print("       If hasEntry=0 or doubleSided never changes, the stamp block")
    print("       doesn't fire -- registry/matching is broken.")
    print("    3. 'MPDiag: matCheck material[N] doubleSided=X' confirms GPU")
    print("       sees the flag. If it always says '=1' or same value across")
    print("       toggles, the flag never made it onto SpectralMaterial.")


if __name__ == "__main__":
    main()
