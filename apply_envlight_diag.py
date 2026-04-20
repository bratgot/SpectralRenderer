#!/usr/bin/env python3
"""
apply_envlight_diag.py -- diagnose the intermittent HDRI dome bug
where avg=(0,0,0) and cdf=no alternating with correct readings on the
same scene.

Plants three tracers:

  1. Immediately after the upstream Iop pixel read (line ~8440),
     sample 5 pixels (center + 4 corners) and log their RGB values.
     If upstream Iop is returning zeros, we see it here.

  2. Immediately before ComputeEnvAverage (line ~8472), log the
     stored envPixels pointer + envWidth + envHeight, plus sample
     3 pixels via the pointer. If the pointer is dangling or
     pointing at zero, we see it here.

  3. Inside SpectralLight::ComputeEnvAverage, log at start: pointer
     value + first 3 pixels. This confirms what the function sees
     vs what the caller thought it set.

Diag output tagged 'EnvDiag:'. Move sliders / scrub timeline until
you see a 'cdf=no' render, then look at the surrounding EnvDiag
lines to pinpoint where zeros entered.

Files touched: src/SpectralRenderIop.cpp, src/SpectralLight.h
Idempotent, CRLF-safe, backs up to .bak_envdiag.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  SpectralRenderIop.cpp: after upstream read + before ComputeEnvAverage
# ============================================================================

IOP_INCLUDE_OLD = """#include <chrono>
#include <cstdlib>
#include <unordered_set>
#include <vector>"""

IOP_INCLUDE_NEW = """#include <chrono>
#include <cstdlib>
#include <unordered_set>
#include <vector>
#include <array>"""


IOP_READ_OLD = """                        texId = _scene->AddTexture(std::move(tex));
                        hasHdri = true;
                        SLOG("SpectralRender: HDRI from input pipe (%dx%d)\\n", W, H);
                    }
"""

IOP_READ_NEW = """                        // Sample 5 pixels from local `tex` BEFORE move into scene.
                        // Center + 4 corners. If these are zero, upstream Iop
                        // returned zeros (bug is upstream).
                        {
                            auto smp = [&](int x, int y) -> std::array<float, 3> {
                                size_t i = (size_t(y) * W + x) * 3;
                                return { tex._pixels[i], tex._pixels[i+1], tex._pixels[i+2] };
                            };
                            auto c = smp(W/2, H/2);
                            auto tl = smp(0, 0);
                            auto br = smp(W-1, H-1);
                            fprintf(stderr,
                                "EnvDiag: PRE-ADD sampled pixels  center=(%.3f,%.3f,%.3f)  "
                                "tl=(%.3f,%.3f,%.3f)  br=(%.3f,%.3f,%.3f)\\n",
                                c[0], c[1], c[2], tl[0], tl[1], tl[2], br[0], br[1], br[2]);
                        }
                        texId = _scene->AddTexture(std::move(tex));
                        hasHdri = true;
                        SLOG("SpectralRender: HDRI from input pipe (%dx%d)\\n", W, H);
                    }
"""


IOP_CEA_OLD = """                hdriDome.envPixels = tex->_pixels.data();
                hdriDome.envRotation = float(el->hdriRotate);
                hdriDome.envShadowSoftness = float(el->hdriShadowSoftness);
                hdriDome.ComputeEnvAverage();
"""

IOP_CEA_NEW = """                hdriDome.envPixels = tex->_pixels.data();
                hdriDome.envRotation = float(el->hdriRotate);
                hdriDome.envShadowSoftness = float(el->hdriShadowSoftness);
                // Sample via hdriDome.envPixels right before ComputeEnvAverage.
                // If these differ from PRE-ADD numbers, the vector reallocated
                // or the texture got corrupted in between.
                {
                    int W = hdriDome.envWidth, H = hdriDome.envHeight;
                    auto smp = [&](int x, int y) -> std::array<float, 3> {
                        size_t i = (size_t(y) * W + x) * 3;
                        return { hdriDome.envPixels[i],
                                 hdriDome.envPixels[i+1],
                                 hdriDome.envPixels[i+2] };
                    };
                    auto c = smp(W/2, H/2);
                    auto tl = smp(0, 0);
                    auto br = smp(W-1, H-1);
                    fprintf(stderr,
                        "EnvDiag: PRE-CEA  envPixels=%p  texels center=(%.3f,%.3f,%.3f)  "
                        "tl=(%.3f,%.3f,%.3f)  br=(%.3f,%.3f,%.3f)\\n",
                        (void*)hdriDome.envPixels,
                        c[0], c[1], c[2], tl[0], tl[1], tl[2], br[0], br[1], br[2]);
                }
                hdriDome.ComputeEnvAverage();
"""


# ============================================================================
#  SpectralLight.h: entry to ComputeEnvAverage
# ============================================================================

LIGHT_OLD = """#include <cmath>
#include <algorithm>
#include <string>
#include <vector>"""

LIGHT_NEW = """#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <array>
#include <cstdio>"""


CEA_OLD = """    void ComputeEnvAverage() {
        envHasCDF = false;
        if (!envPixels || envWidth <= 0 || envHeight <= 0) return;
"""

CEA_NEW = """    void ComputeEnvAverage() {
        envHasCDF = false;
        if (!envPixels || envWidth <= 0 || envHeight <= 0) {
            fprintf(stderr, "EnvDiag: CEA  EARLY RETURN  envPixels=%p W=%d H=%d\\n",
                (const void*)envPixels, envWidth, envHeight);
            return;
        }
        {
            int W = envWidth, H = envHeight;
            auto smp = [&](int x, int y) -> std::array<float,3> {
                size_t i = (size_t(y) * W + x) * 3;
                return { envPixels[i], envPixels[i+1], envPixels[i+2] };
            };
            auto c = smp(W/2, H/2);
            auto tl = smp(0, 0);
            auto br = smp(W-1, H-1);
            fprintf(stderr,
                "EnvDiag: CEA     envPixels=%p  texels center=(%.3f,%.3f,%.3f)  "
                "tl=(%.3f,%.3f,%.3f)  br=(%.3f,%.3f,%.3f)\\n",
                (const void*)envPixels,
                c[0], c[1], c[2], tl[0], tl[1], tl[2], br[0], br[1], br[2]);
        }
"""


EDITS_IOP = [
    (
        "Iop: add <array> include",
        IOP_INCLUDE_OLD, IOP_INCLUDE_NEW,
        "#include <array>"
    ),
    (
        "Iop: log pre-AddTexture pixel samples",
        IOP_READ_OLD, IOP_READ_NEW,
        "EnvDiag: PRE-ADD sampled pixels"
    ),
    (
        "Iop: log envPixels pointer + samples before ComputeEnvAverage",
        IOP_CEA_OLD, IOP_CEA_NEW,
        "EnvDiag: PRE-CEA"
    ),
]
EDITS_LIGHT = [
    (
        "SpectralLight: add <array> + <cstdio> includes",
        LIGHT_OLD, LIGHT_NEW,
        "#include <array>"
    ),
    (
        "SpectralLight: log pointer + samples inside ComputeEnvAverage",
        CEA_OLD, CEA_NEW,
        "EnvDiag: CEA     envPixels"
    ),
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
    bak = ".bak_envdiag"
    ok = True
    ok &= process(args.src / "SpectralRenderIop.cpp", EDITS_IOP,   args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralLight.h",       EDITS_LIGHT, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Scrub timeline / move sliders to provoke an intermittent")
    print("render. Look at stderr for 'EnvDiag:' lines. When cdf=no appears,")
    print("compare the pixel samples from PRE-ADD, PRE-CEA, and CEA. Deviation")
    print("tells us where zeros crept in.")


if __name__ == "__main__":
    main()
