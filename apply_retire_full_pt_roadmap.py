"""
apply_retire_full_pt_roadmap.py

Retires the "Full path tracing" near-term-backlog item from ROADMAP.txt.

The roadmap item (lines 102-106 as of 2026-04-23) was authored when the
integrator was a hybrid direct-lighting + limited-bounce indirect. The
work it describes has since landed across Phases 14-17 and the GPU
caching work in Phase 23.1:

    - Unified MIS path tracing with power heuristic (beta=2) on CPU
    - Same architecture mirrored in the OptiX kernel
    - Russian roulette after bounce 1 on both paths
    - Path regularization for rough glass (firefly reduction)
    - HDRI CDF-based environment importance sampling with proper MIS
      against BSDF-sampled-environment paths

Verified 2026-04-23 by reading SpectralIntegrator.cpp:1899-2141 and
SpectralGPUKernel.cu's __raygen__spectral bounce loop. The remaining
hardcoded 0.5f weights in the GPU kernel are intentional fallbacks for
non-CDF dome lights, where pdf-based MIS is mathematically undefined --
the comments at those sites say so explicitly.

Rather than just deleting the bullet, this edit replaces it with a
two-line retirement note that preserves the audit trail. Future-us
reading the roadmap should be able to see that this was considered
and resolved.

Usage:
    python apply_retire_full_pt_roadmap.py [path-to-ROADMAP.txt]
"""

import sys
import shutil
from pathlib import Path

DEFAULT_PATH = Path(
    r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\ROADMAP.txt"
)

EDITS = [
    (
        "### Full path tracing\n"
        "\n"
        "Currently a hybrid direct-lighting + limited-bounce indirect. Unified\n"
        "path tracing with MIS would simplify the integrator, improve convergence\n"
        "for complex lighting, and match industry expectations.\n"
        "\n"
        "### SpectralSurface PBR expansion (Phase 21)\n",

        "### Full path tracing -- retired 2026-04-23\n"
        "\n"
        "Already done. Verified by reading SpectralIntegrator.cpp:1899-2141 and\n"
        "the SpectralGPUKernel.cu raygen bounce loop: unified MIS path tracing\n"
        "with power heuristic (beta=2), Russian roulette after bounce 1, path\n"
        "regularization for rough glass, HDRI CDF importance sampling with\n"
        "proper MIS against BSDF-sampled-environment paths. The hardcoded 0.5f\n"
        "weights in the GPU kernel's non-CDF branches are intentional fallbacks\n"
        "for uniform domes where pdf-based MIS is mathematically undefined.\n"
        "\n"
        "### SpectralSurface PBR expansion (Phase 21)\n",

        "retire Full PT roadmap item",
    ),
]


def main() -> int:
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PATH
    if not target.is_file():
        print(f"ERROR: not a file: {target}")
        return 1

    with open(target, "rb") as f:
        raw = f.read()
    has_crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")

    new_text = text
    applied = 0
    already = 0

    for old, new, label in EDITS:
        old_n = new_text.count(old)
        if old_n == 0:
            if new_text.count(new) >= 1:
                print(f"  [{label}] already applied")
                already += 1
                continue
            print(f"  [{label}] ERROR: anchor not found")
            return 2
        if old_n > 1:
            print(f"  [{label}] ERROR: anchor matches {old_n} times -- ambiguous")
            return 3
        new_text = new_text.replace(old, new, 1)
        applied += 1
        print(f"  [{label}] applied")

    if applied == 0:
        print(f"All {already} edits already applied. Nothing to do.")
        return 0

    bak = target.with_suffix(target.suffix + ".bak")
    shutil.copy2(target, bak)
    print(f"Backup: {bak}")

    out = new_text.replace("\n", "\r\n") if has_crlf else new_text
    with open(target, "wb") as f:
        f.write(out.encode("utf-8"))
    print(f"Wrote: {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
