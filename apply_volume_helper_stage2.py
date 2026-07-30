"""
apply_volume_helper_stage2.py  (Stage 2)

Plumbs _MarchVolumesAlongSegment into the bounce loop in _ShadeSpectral,
fixing the volumetric refraction bug. Also propagates `pixIdx` and
`camera` through the function signature -- the helper needs them for
shadow-ray jitter and noShadowCastMatIds, neither of which were in
_ShadeSpectral's scope.

Five coordinated edits across two files:

    1. SpectralIntegrator.h:
       Update _ShadeSpectral signature: add `int pixIdx` and
       `const SpectralCamera& camera`.

    2. SpectralIntegrator.cpp:
       Update _ShadeSpectral definition signature to match.

    3. SpectralIntegrator.cpp:
       Update the single call site in RenderFrame to pass pixIdx, camera.

    4. SpectralIntegrator.cpp:
       Add bounce-hit volume march before the existing Beer-Lambert
       material-interior absorption block.

    5. SpectralIntegrator.cpp:
       Add bounce-miss volume march at the top of the dome-light MIS
       branch (before the existing dome MIS code).

This is the actual fix. After this lands, refraction rays through
glass containing fire/smoke will correctly integrate the volume.

Stage 3 (performance knob) is NOT in this script. Run, render the
test scene, decide whether perf is acceptable. If too slow, follow
up with a separate quality-knob script.

Usage:
    python apply_volume_helper_stage2.py [path-to-src]
"""

import sys
import shutil
from pathlib import Path

DEFAULT_SRC = Path(
    r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\src"
)


# -------------------------------------------------------------------------
# Edit 1: header signature update
# -------------------------------------------------------------------------

H_OLD = """\
    static float _ShadeSpectral(const SpectralTriangle& tri,
                                 double u, double v, float lambda,
                                 const SpectralMaterial& mat,
                                 const SpectralScene& scene,
                                 const GfVec3f& hitPos,
                                 const GfVec3f& rayDir,
                                 int maxBounces,
                                 unsigned int& rngSeed,
                                 const SpectralBVH& bvh,
                                 float rayTime = 0.f,
                                 ShadeComponents* comps = nullptr,
                                 const SpectralPhotonMap* photonMap = nullptr,
                                 float gatherRadius = 0.5f,
                                 const SpectralVolume* const* volumes = nullptr,
                                 int numVolumes = 0);
"""

H_NEW = """\
    static float _ShadeSpectral(const SpectralTriangle& tri,
                                 double u, double v, float lambda,
                                 const SpectralMaterial& mat,
                                 const SpectralScene& scene,
                                 const GfVec3f& hitPos,
                                 const GfVec3f& rayDir,
                                 int maxBounces,
                                 unsigned int& rngSeed,
                                 const SpectralBVH& bvh,
                                 float rayTime = 0.f,
                                 ShadeComponents* comps = nullptr,
                                 const SpectralPhotonMap* photonMap = nullptr,
                                 float gatherRadius = 0.5f,
                                 const SpectralVolume* const* volumes = nullptr,
                                 int numVolumes = 0,
                                 int pixIdx = 0,
                                 const SpectralCamera* camera = nullptr);
"""


# -------------------------------------------------------------------------
# Edit 2: cpp definition signature update
# -------------------------------------------------------------------------

CPP_DEF_OLD = """\
float SpectralIntegrator::_ShadeSpectral(
    const SpectralTriangle& tri, double u, double v, float lambda,
    const SpectralMaterial& mat, const SpectralScene& scene,
    const GfVec3f& hitPos, const GfVec3f& rayDir, int maxBounces,
    unsigned int& rngSeed, const SpectralBVH& bvh, float rayTime,
    ShadeComponents* comps, const SpectralPhotonMap* photonMap,
    float gatherRadius,
    const SpectralVolume* const* volumes, int numVolumes)
{
"""

CPP_DEF_NEW = """\
float SpectralIntegrator::_ShadeSpectral(
    const SpectralTriangle& tri, double u, double v, float lambda,
    const SpectralMaterial& mat, const SpectralScene& scene,
    const GfVec3f& hitPos, const GfVec3f& rayDir, int maxBounces,
    unsigned int& rngSeed, const SpectralBVH& bvh, float rayTime,
    ShadeComponents* comps, const SpectralPhotonMap* photonMap,
    float gatherRadius,
    const SpectralVolume* const* volumes, int numVolumes,
    int pixIdx, const SpectralCamera* camera)
{
"""


# -------------------------------------------------------------------------
# Edit 3: call site in RenderFrame
# -------------------------------------------------------------------------

CPP_CALL_OLD = """\
                                radiance = _ShadeSpectral(
                                    *hit.tri,
                                    static_cast<double>(hit.u),
                                    static_cast<double>(hit.v),
                                    lambda, mat, scene, hitPos, rayDir,
                                    shadeBounces, bounceSeed, bvh, rayTime, &comps,
                                    photonMap, gatherRadius, volumes, numVolumes);
"""

CPP_CALL_NEW = """\
                                radiance = _ShadeSpectral(
                                    *hit.tri,
                                    static_cast<double>(hit.u),
                                    static_cast<double>(hit.v),
                                    lambda, mat, scene, hitPos, rayDir,
                                    shadeBounces, bounceSeed, bvh, rayTime, &comps,
                                    photonMap, gatherRadius, volumes, numVolumes,
                                    int(pixIdx), &camera);
"""


# -------------------------------------------------------------------------
# Edit 4: bounce-hit volume march. Insert before existing Beer-Lambert
# material-interior absorption block.
# -------------------------------------------------------------------------

CPP_BHIT_OLD = """\
        SpectralBVH::Hit bounceHit = bvh.Intersect(bounceRay, rayTime);

        // Beer-Lambert absorption: attenuate for distance traveled inside volume
        if (insideVolumeMat && bounceHit.valid()) {
            float T = insideVolumeMat->SpectralTransmittance(lambda, float(bounceHit.t));
            pathThroughput *= T;
            if (pathThroughput < 1e-6f) break;
        }
"""

CPP_BHIT_NEW = """\
        SpectralBVH::Hit bounceHit = bvh.Intersect(bounceRay, rayTime);

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

        // Beer-Lambert absorption: attenuate for distance traveled inside volume
        if (insideVolumeMat && bounceHit.valid()) {
            float T = insideVolumeMat->SpectralTransmittance(lambda, float(bounceHit.t));
            pathThroughput *= T;
            if (pathThroughput < 1e-6f) break;
        }
"""


# -------------------------------------------------------------------------
# Edit 5: bounce-miss volume march. Insert at the top of the
# `if (!bounceHit.valid())` branch.
# -------------------------------------------------------------------------

CPP_BMISS_OLD = """\
        if (!bounceHit.valid()) {
            // Miss — check dome lights for BSDF-side MIS contribution
            if (!scene.GetLights().empty()) {
"""

CPP_BMISS_NEW = """\
        if (!bounceHit.valid()) {
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

            // Miss — check dome lights for BSDF-side MIS contribution
            if (!scene.GetLights().empty()) {
"""


FILE_EDITS = {
    "SpectralIntegrator.h":   [(H_OLD, H_NEW, "header signature")],
    "SpectralIntegrator.cpp": [
        (CPP_DEF_OLD,   CPP_DEF_NEW,   "cpp signature"),
        (CPP_CALL_OLD,  CPP_CALL_NEW,  "call site"),
        (CPP_BHIT_OLD,  CPP_BHIT_NEW,  "bounce-hit volume march"),
        (CPP_BMISS_OLD, CPP_BMISS_NEW, "bounce-miss volume march"),
    ],
}


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
