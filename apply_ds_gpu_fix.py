#!/usr/bin/env python3
"""
apply_ds_gpu_fix.py -- repair missing GPU-side pieces from
apply_meshprops_fields.py. Root cause of "toggling doubleSided has no
effect on GPU" was discovered to be FOUR silent misses:

  1. GPUMaterial struct (SpectralGPUParams.h) doesn't have a doubleSided
     field -- my original anchor expected sssRadius to be last, but
     clearcoat+clearcoatRoughness land there in the user's Phase 3 state.
  2. matCheck (SpectralGPU.cpp checksum) doesn't include doubleSided.
     Result: GPU cache says "no change" and reuses the previous upload
     even when the flag flipped.
  3+4. gpuMats[i].doubleSided is NOT copied from mats[i].doubleSided at
     either of the two material-upload sites. So even if the cache were
     invalidated, the GPU struct wouldn't carry the new value.

Anchors in this patch use clearcoatRoughness as the landmark so they
match the user's actual on-disk state.

Idempotent via marker, CRLF-safe, backup .bak_dsgpufix.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  Edit 1: SpectralGPUParams.h -- add doubleSided field to GPUMaterial.
#  Anchor on the clearcoatRoughness line which is now the last field.
# ============================================================================

PARAMS_OLD = """    float  clearcoat;
    float  clearcoatRoughness;
};
"""

PARAMS_NEW = """    float  clearcoat;
    float  clearcoatRoughness;
    // Double-sided flag. When 0, back-facing primary hits are treated
    // as misses (ray passes through). Matches CPU and USD doubleSided.
    int    doubleSided;
};
"""


# ============================================================================
#  Edit 2: SpectralGPU.cpp -- extend matCheck to cover doubleSided.
#  Anchor on the metalType line.
# ============================================================================

CHK_OLD = """            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
"""

CHK_NEW = """            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
            matCheck ^= (mats[i].doubleSided ? 1u : 0u) * 2246822519u;
"""


# ============================================================================
#  Edit 3 + 4: copy doubleSided into GPUMaterial at both upload sites.
#  The clearcoatRoughness copy is the landmark just before the closing
#  brace of each copy loop.
#
#  Site 1: 16-space indent (inside an outer block)
#  Site 2: 12-space indent
# ============================================================================

COPY1_OLD = """                gpuMats[i].clearcoatRoughness = mats[i].clearcoatRoughness;
            }
            const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
"""

COPY1_NEW = """                gpuMats[i].clearcoatRoughness = mats[i].clearcoatRoughness;
                gpuMats[i].doubleSided     = mats[i].doubleSided ? 1 : 0;
            }
            const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
"""


COPY2_OLD = """            gpuMats[i].clearcoatRoughness = mats[i].clearcoatRoughness;
        }
        const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
"""

COPY2_NEW = """            gpuMats[i].clearcoatRoughness = mats[i].clearcoatRoughness;
            gpuMats[i].doubleSided     = mats[i].doubleSided ? 1 : 0;
        }
        const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
"""


EDITS_PARAMS = [
    ("Add doubleSided field to GPUMaterial struct",
     PARAMS_OLD, PARAMS_NEW,
     "Double-sided flag. When 0, back-facing primary hits are treated"),
]
EDITS_GPU = [
    ("Checksum: include doubleSided so GPU cache invalidates on toggle",
     CHK_OLD, CHK_NEW,
     "(mats[i].doubleSided ? 1u : 0u) * 2246822519u"),
    ("Copy at first upload site",
     COPY1_OLD, COPY1_NEW,
     "                gpuMats[i].doubleSided     = mats[i].doubleSided ? 1 : 0;"),
    ("Copy at second upload site",
     COPY2_OLD, COPY2_NEW,
     "            gpuMats[i].doubleSided     = mats[i].doubleSided ? 1 : 0;\n        }"),
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
    bak = ".bak_dsgpufix"
    ok = True
    ok &= process(args.src / "SpectralGPUParams.h", EDITS_PARAMS, args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPU.cpp",     EDITS_GPU,    args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. After this:")
    print("  - GPUMaterial struct has a doubleSided field")
    print("  - Toggling doubleSided on SpectralMeshProperties invalidates")
    print("    GPU material cache (check stderr for 're-uploaded N materials')")
    print("  - The GPU kernel reads the correct flag and treats back-facing")
    print("    hits as miss when the mesh is single-sided")


if __name__ == "__main__":
    main()
