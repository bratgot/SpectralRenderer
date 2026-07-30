"""
apply_volume_helper_body.py  (Stage 1b)

Second half of the Shape 2 refactor for volumetric refraction.

Two coordinated edits in SpectralIntegrator.cpp:

    1. Replace the Stage 1a stub body of _MarchVolumesAlongSegment with
       the real body lifted from the primary-ray inline volume code.

    2. Replace the inline volume block in RenderFrame (originally
       lines 770-1163) with an alpha dispatcher that routes:
         - any-volume-is-spectralVolumes -> per-volume helper calls,
           preserving spectralVolumes-mode per-wavelength composite
         - all volumes non-spectralVolumes (default) -> single helper
           call that marches all volumes inside, returns one
           (rgb, transmittance, firstDenseT) tuple

Goal: byte-identical rendered output to pre-Stage-1b. The helper body
is the inline code with parameter renamings only -- no algorithmic
change. Both paths preserve the same arithmetic; the only difference
is that the non-spectralVolumes path now lives inside a function call
instead of inline.

The helper body and replacement block live in sibling .txt files
because embedding ~400 lines of C++ in a Python string is hostile
to maintenance:
    _march_volumes_body.txt           -- helper definition
    _render_frame_replacement.txt     -- alpha dispatcher

This script reads both at runtime. All three files must be co-located.

Usage:
    python apply_volume_helper_body.py [path-to-SpectralIntegrator.cpp]
"""

import sys
import shutil
from pathlib import Path

DEFAULT_PATH = Path(
    r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\src\SpectralIntegrator.cpp"
)

SCRIPT_DIR = Path(__file__).parent
HELPER_BODY_FILE  = SCRIPT_DIR / "_march_volumes_body.txt"
REPLACEMENT_FILE  = SCRIPT_DIR / "_render_frame_replacement.txt"


# Anchor 1: the Stage 1a stub body. Replace with the real body.
STUB_OLD = '''SpectralIntegrator::VolumeMarchResult
SpectralIntegrator::_MarchVolumesAlongSegment(
    const GfVec3f& origin,
    const GfVec3f& dir,
    float maxT,
    float lambda,
    unsigned int seed,
    int pixIdx,
    const SpectralVolume* const* volumes,
    int numVolumes,
    const SpectralScene& scene,
    const SpectralBVH& bvh,
    const SpectralCamera& camera)
{
    // Suppress unused-parameter warnings until the body lands in Stage 1b.
    (void)origin; (void)dir; (void)maxT; (void)lambda;
    (void)seed; (void)pixIdx;
    (void)volumes; (void)numVolumes;
    (void)scene; (void)bvh; (void)camera;
    return VolumeMarchResult{};
}
'''


def main() -> int:
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PATH
    if not target.is_file():
        print(f"ERROR: not a file: {target}")
        return 1
    if not HELPER_BODY_FILE.is_file():
        print(f"ERROR: helper body file not found: {HELPER_BODY_FILE}")
        return 1
    if not REPLACEMENT_FILE.is_file():
        print(f"ERROR: replacement file not found: {REPLACEMENT_FILE}")
        return 1

    helper_body = HELPER_BODY_FILE.read_text(encoding="utf-8")
    replacement = REPLACEMENT_FILE.read_text(encoding="utf-8")

    with open(target, "rb") as f:
        raw = f.read()
    has_crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")

    # ---- Edit 1: stub -> real body ----
    if STUB_OLD not in text:
        if helper_body.rstrip() in text:
            print("  [helper body] already applied (real body present)")
            edit1_applied = False
        else:
            print("  [helper body] ERROR: neither stub nor real body found")
            print("                 (was Stage 1a applied? was the file modified?)")
            return 2
    else:
        if text.count(STUB_OLD) > 1:
            print(f"  [helper body] ERROR: stub matches {text.count(STUB_OLD)} times")
            return 3
        text = text.replace(STUB_OLD, helper_body, 1)
        edit1_applied = True
        print("  [helper body] applied")

    # ---- Edit 2: inline block -> alpha dispatcher ----
    # The inline block to replace runs from the "// Volume ray marching" comment
    # through the closing brace of `for (int vi)`. Read the canonical OLD from
    # the original file's lines 770-1163. Since this script may run after
    # Stage 1a (which doesn't touch this block), the OLD anchor is identical
    # to what's on disk pre-Stage-1b.
    OLD_INLINE = read_canonical_inline_block(target, edit1_applied, text)
    if OLD_INLINE is None:
        # Already replaced (idempotent re-run)
        if replacement.rstrip() in text:
            print("  [render frame] already applied (replacement present)")
            edit2_applied = False
        else:
            print("  [render frame] ERROR: neither inline block nor replacement found")
            print("                 (file structure unexpected)")
            return 4
    else:
        if text.count(OLD_INLINE) != 1:
            n = text.count(OLD_INLINE)
            print(f"  [render frame] ERROR: inline block matches {n} times (expected 1)")
            return 5
        text = text.replace(OLD_INLINE, replacement, 1)
        edit2_applied = True
        print("  [render frame] applied")

    if not edit1_applied and not edit2_applied:
        print("All edits already applied. Nothing to do.")
        return 0

    bak = target.with_suffix(target.suffix + ".bak")
    shutil.copy2(target, bak)
    print(f"Backup: {bak}")

    out = text.replace("\n", "\r\n") if has_crlf else text
    with open(target, "wb") as f:
        f.write(out.encode("utf-8"))
    print(f"Wrote: {target}")
    return 0


def read_canonical_inline_block(target: Path, edit1_applied: bool, text: str):
    """Return the inline-volume-block string to anchor against, or None
    if the replacement is already in place.

    The inline block starts at the // Volume ray marching comment and ends
    at the closing brace of the outer `for (int vi)` loop.
    """
    start_marker = "                            // Volume ray marching"
    if start_marker not in text:
        return None

    start_idx = text.find(start_marker)
    # Find the matching `} else {  // ...` that ends the spectralVolumes block,
    # then walk forward to the line `                            }` that closes
    # the outer if/for. Simpler: anchor on the unique sentinel
    # `finalVolTrans *= volTransmittance;  // multiply, don't overwrite\n`
    # then find the next two lines (closes the inner `if (volume->...)`,
    # closes the `for (vi)` block).
    sentinel = "                                    finalVolTrans *= volTransmittance;  // multiply, don't overwrite\n"
    sentinel_idx = text.find(sentinel, start_idx)
    if sentinel_idx == -1:
        return None
    # After the sentinel: `                                }\n                            }\n`
    end_pattern = "                                }\n                            }\n"
    after_sent = sentinel_idx + len(sentinel)
    if not text[after_sent:].startswith(end_pattern):
        return None
    end_idx = after_sent + len(end_pattern)
    return text[start_idx:end_idx]


if __name__ == "__main__":
    sys.exit(main())
