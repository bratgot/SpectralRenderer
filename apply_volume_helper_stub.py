"""
apply_volume_helper_stub.py  (Stage 1a)

First half of the Shape 2 refactor for volumetric refraction.

Adds wiring for a new private helper:
    SpectralIntegrator::_MarchVolumesAlongSegment

This is the STUB stage -- the helper exists, compiles, and returns a
no-op result (zero radiance, full transmittance). No call sites change
yet. No render output should change.

Goal: verify the build picks up the new symbol cleanly. If this
compiles + links + the test scene renders identically, Stage 1b
(filling in the helper body and routing the primary-ray inline code
through it) is safe to attempt.

Two coordinated edits across two files:

    1. SpectralIntegrator.h:
       - Forward-declare the SpectralBVH type (already included via
         SpectralBVH.h, this is just for clarity).
       - Add VolumeMarchResult struct in private section.
       - Add _MarchVolumesAlongSegment static method declaration.

    2. SpectralIntegrator.cpp:
       - Add the stub implementation right after _Hash (~line 2465).

Usage:
    python apply_volume_helper_stub.py [path-to-src]
"""

import sys
import shutil
from pathlib import Path

DEFAULT_SRC = Path(
    r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\src"
)


# -------------------------------------------------------------------------
# Header edit -- add the struct + declaration in the private section
# -------------------------------------------------------------------------

H_OLD = """\
    // Simple hash-based RNG for per-pixel, per-sample jitter
    static float _Hash(unsigned int seed);
};
"""

H_NEW = """\
    // Simple hash-based RNG for per-pixel, per-sample jitter
    static float _Hash(unsigned int seed);

    // -----------------------------------------------------------------------
    // Volume marching along a ray segment.
    //
    // Returns the accumulated RGB radiance and transmittance for the
    // segment [origin, origin + dir*maxT]. Used by both the primary-ray
    // pass (RenderFrame) and bounce rays (_ShadeSpectral) so that
    // refraction through glass containing fire/smoke shows the volume
    // correctly. Mirrors the GPU kernel's marchVolume.
    //
    // The returned firstDenseT is the distance to the first non-empty
    // voxel encountered, or 1e30 if none -- used by the primary pass
    // for the volume depth AOV; bounce callers may discard it.
    // -----------------------------------------------------------------------
    struct VolumeMarchResult {
        GfVec3f rgb           = GfVec3f(0.f);
        float   transmittance = 1.f;
        float   firstDenseT   = 1e30f;
    };

    static VolumeMarchResult _MarchVolumesAlongSegment(
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
        const SpectralCamera& camera);
};
"""

# -------------------------------------------------------------------------
# CPP edit -- add the stub implementation right after _Hash
# -------------------------------------------------------------------------

CPP_OLD = """\
float SpectralIntegrator::_Hash(unsigned int seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / float(0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// ComputeAO\
"""

CPP_NEW = """\
float SpectralIntegrator::_Hash(unsigned int seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / float(0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// _MarchVolumesAlongSegment -- volume integration over a ray segment.
//
//   STAGE 1a STUB. Returns a no-op result. The full implementation lifts
//   the primary-ray inline volume-marching code (currently in RenderFrame,
//   ~lines 779-1163) into this helper so the bounce loop in _ShadeSpectral
//   can call it too. Until Stage 1b lands, no call sites use this method.
//
//   Parameters:
//     origin, dir, maxT  Segment to march. dir need not be normalized.
//     lambda             For chromatic extinction selection.
//     seed               Outer-loop seed; helper derives jitter from it.
//     pixIdx             For shadow-ray jitter (matches existing pattern).
//     volumes, numVolumes  Volume list to march through.
//     scene              Lights + materials for in-scattering.
//     bvh                Geometry occlusion of light rays inside volumes.
//     camera             Carries noShadowCastMatIds.
// ---------------------------------------------------------------------------
SpectralIntegrator::VolumeMarchResult
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

// ---------------------------------------------------------------------------
// ComputeAO\
"""


def apply_to_file(path: Path, edits) -> int:
    if not path.is_file():
        print(f"  ERROR: not a file: {path}")
        return 1

    with open(path, "rb") as f:
        raw = f.read()

    has_crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")

    new_text = text
    applied = 0
    already = 0

    for old, new, label in edits:
        old_n = new_text.count(old)
        if old_n == 0:
            if new_text.count(new) >= 1:
                print(f"    [{label}] already applied")
                already += 1
                continue
            print(f"    [{label}] ERROR: anchor not found")
            return 2
        if old_n > 1:
            print(f"    [{label}] ERROR: matches {old_n} times -- ambiguous")
            return 3
        new_text = new_text.replace(old, new, 1)
        applied += 1
        print(f"    [{label}] applied")

    if applied == 0:
        print(f"  All {already} edits already applied in {path.name}.")
        return 0

    bak = path.with_suffix(path.suffix + ".bak")
    shutil.copy2(path, bak)
    print(f"  Backup: {bak}")
    out = new_text.replace("\n", "\r\n") if has_crlf else new_text
    with open(path, "wb") as f:
        f.write(out.encode("utf-8"))
    print(f"  Wrote: {path}")
    return 0


FILE_EDITS = {
    "SpectralIntegrator.h":   [(H_OLD,   H_NEW,   "add VolumeMarchResult + decl")],
    "SpectralIntegrator.cpp": [(CPP_OLD, CPP_NEW, "add _MarchVolumesAlongSegment stub")],
}


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    if not src.is_dir():
        print(f"ERROR: not a directory: {src}")
        return 1

    for filename, edits in FILE_EDITS.items():
        print(f"== {filename} ==")
        rc = apply_to_file(src / filename, edits)
        if rc != 0:
            return rc
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
