"""
apply_disable_bounce_vol_diag.py

Diagnostic script: wraps the two new bounce-volume-marching blocks in
#if 0 / #endif so they don't execute. Used to localize the
"refraction broken after Stage 2" bug.

Two scenarios after running this + rebuild + test:
    - glass refraction works again -> bug is INSIDE these blocks
    - glass refraction still broken -> bug is in the signature plumbing
      (Stage 2 edits 1-3) and these blocks are not the cause

To revert: just `git checkout src/SpectralIntegrator.cpp` once the
diagnosis is done.

Usage:
    python apply_disable_bounce_vol_diag.py [path-to-SpectralIntegrator.cpp]
"""

import sys
import shutil
from pathlib import Path

DEFAULT_PATH = Path(
    r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\src\SpectralIntegrator.cpp"
)

# -------------------------------------------------------------------------
# Edit 1: bounce-hit block. Wrap with #if 0 / #endif.
# -------------------------------------------------------------------------

OLD_BHIT = """\
        // Volume marching along bounce-ray segment. Refraction rays through
        // glass containing fire/smoke now correctly integrate the volume.
        // Mirrors GPU kernel marchVolume call after each bounce hit.
        if (numVolumes > 0 && bounceHit.valid() && camera) {
            auto vr = _MarchVolumesAlongSegment(
                GfVec3f(bounceRay.GetStartPoint()),
                GfVec3f(bounceRay.GetDirection()),
                float(bounceHit.t),
                lambda, rngSeed + unsigned(bounce)*97u, pixIdx,
                volumes, numVolumes, scene, bvh, *camera);
            int sc = (lambda < 500.f) ? 2 : (lambda < 580.f) ? 1 : 0;
            float volSpec = vr.rgb[sc];
            radiance += pathThroughput * volSpec;
            pathThroughput *= vr.transmittance;
            if (comps) comps->indirect += pathThroughput * volSpec;
        }
"""

NEW_BHIT = """\
        // Volume marching along bounce-ray segment. DISABLED for diagnosis.
#if 0
        if (numVolumes > 0 && bounceHit.valid() && camera) {
            auto vr = _MarchVolumesAlongSegment(
                GfVec3f(bounceRay.GetStartPoint()),
                GfVec3f(bounceRay.GetDirection()),
                float(bounceHit.t),
                lambda, rngSeed + unsigned(bounce)*97u, pixIdx,
                volumes, numVolumes, scene, bvh, *camera);
            int sc = (lambda < 500.f) ? 2 : (lambda < 580.f) ? 1 : 0;
            float volSpec = vr.rgb[sc];
            radiance += pathThroughput * volSpec;
            pathThroughput *= vr.transmittance;
            if (comps) comps->indirect += pathThroughput * volSpec;
        }
#endif
"""

# -------------------------------------------------------------------------
# Edit 2: bounce-miss block. Wrap inner if with #if 0 / #endif.
# -------------------------------------------------------------------------

OLD_BMISS = """\
            // Volume marching along bounce-miss segment (ray escapes to env).
            // Mirrors GPU kernel marchVolume in the bounce-miss branch.
            if (numVolumes > 0 && camera) {
                auto vr = _MarchVolumesAlongSegment(
                    GfVec3f(bounceRay.GetStartPoint()),
                    GfVec3f(bounceRay.GetDirection()),
                    1e30f,
                    lambda, rngSeed + unsigned(bounce)*97u, pixIdx,
                    volumes, numVolumes, scene, bvh, *camera);
                int sc = (lambda < 500.f) ? 2 : (lambda < 580.f) ? 1 : 0;
                float volSpec = vr.rgb[sc];
                radiance += pathThroughput * volSpec;
                pathThroughput *= vr.transmittance;
                if (comps) comps->indirect += pathThroughput * volSpec;
            }
"""

NEW_BMISS = """\
            // Volume marching along bounce-miss segment. DISABLED for diagnosis.
#if 0
            if (numVolumes > 0 && camera) {
                auto vr = _MarchVolumesAlongSegment(
                    GfVec3f(bounceRay.GetStartPoint()),
                    GfVec3f(bounceRay.GetDirection()),
                    1e30f,
                    lambda, rngSeed + unsigned(bounce)*97u, pixIdx,
                    volumes, numVolumes, scene, bvh, *camera);
                int sc = (lambda < 500.f) ? 2 : (lambda < 580.f) ? 1 : 0;
                float volSpec = vr.rgb[sc];
                radiance += pathThroughput * volSpec;
                pathThroughput *= vr.transmittance;
                if (comps) comps->indirect += pathThroughput * volSpec;
            }
#endif
"""

EDITS = [
    (OLD_BHIT,  NEW_BHIT,  "disable bounce-hit volume march"),
    (OLD_BMISS, NEW_BMISS, "disable bounce-miss volume march"),
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
    print()
    print("Diagnostic applied. Now:")
    print("  1. Rebuild: .\\build.ps1")
    print("  2. Render the glass scene")
    print("  3. Tell me what you see -- works or still broken")
    print("To revert: git checkout src/SpectralIntegrator.cpp")
    return 0


if __name__ == "__main__":
    sys.exit(main())
