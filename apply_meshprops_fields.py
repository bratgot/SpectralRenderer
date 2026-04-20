#!/usr/bin/env python3
"""
apply_meshprops_fields.py -- wire up doubleSided, orientation, purpose
from SpectralMeshProperties to the CPU and GPU render paths.

Registry already carries these fields (2026-04-19 session). Hash
coverage already includes them. What was missing: the consumer side
never actually read them. This patch closes that.

Semantics:
  doubleSided=false -> back-facing primary/bounce hits return 0 radiance
                       (effectively transparent from the back side).
                       Default true preserves current behaviour.
                       Per-material flag; if two meshes share a material
                       with different doubleSided settings, last-one-wins.
  orientation=1     -> XOR into the existing flipNormals pass. Effectively
                       treats left-handed orientation as equivalent to
                       flip-normals. Matches USD UsdGeomMesh orientation.
  purpose=2 (proxy) -> mesh is skipped from render (still visible in
           purpose=3 (guide)   viewport via the Drafting overlay path).

Files touched (10 edits, 6 files):
  SpectralMaterial.h        add bool doubleSided = true;
  SpectralRenderIop.cpp     3 locals, skip on purpose, stamp on mat,
                            XOR orientation into flipNormals
  SpectralIntegrator.cpp    gate primary-/bounce-miss on !doubleSided
  SpectralGPUParams.h       add doubleSided to GPUMaterial
  SpectralGPU.cpp           copy field into GPU materials + checksum it
  SpectralGPUKernel.cu      gate primary normal flip on doubleSided

Idempotent via marker, CRLF-safe, backup .bak_mpfields.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  SpectralMaterial.h -- add doubleSided field
# ============================================================================

MAT_OLD = """    // Shadow catcher
    bool    isShadowCatcher = false; // receives shadows only, transparent where lit
"""

MAT_NEW = """    // Shadow catcher
    bool    isShadowCatcher = false; // receives shadows only, transparent where lit

    // Two-sided geometry. When false, back-facing hits are treated as
    // misses -- rays pass through the backside unimpeded. Matches USD
    // UsdGeomMesh double_sided attribute semantics and the Nuke
    // SpectralMeshProperties knob. Last-write-wins if a material is
    // shared across meshes with different doubleSided settings.
    bool    doubleSided = true;
"""


# ============================================================================
#  SpectralRenderIop.cpp -- four edits
#  (a) add locals for the three fields next to existing meshProps* locals
#  (b) populate them from matched->
#  (c) skip the mesh on purpose=proxy/guide
#  (d) XOR orientation into the existing flipNormals application
#  (e) stamp doubleSided onto the resolved material at each site
#
#  For (e) we hit multiple isShadowCatcher stamp sites. Rather than
#  thread doubleSided into every one, we stamp after-the-fact: right
#  before the mesh data is committed to the scene, walk every material
#  ID used by this mesh's triangles and write doubleSided into them.
#  That keeps the patch bounded to the mesh-loop scope.
# ============================================================================

IOP_LOCALS_OLD = """        bool    meshPropsVisible         = true;
        bool    meshPropsCastsShadows    = true;
        bool    meshPropsReceivesShadows = true;
        bool    meshPropsHasEntry        = false;
"""

IOP_LOCALS_NEW = """        bool    meshPropsVisible         = true;
        bool    meshPropsCastsShadows    = true;
        bool    meshPropsReceivesShadows = true;
        bool    meshPropsHasEntry        = false;
        bool    meshPropsDoubleSided     = true;   // false -> backface = miss
        int     meshPropsOrientation     = 0;      // 0=right-handed, 1=left (XOR into flipNormals)
        int     meshPropsPurpose         = 0;      // 0=default, 1=render, 2=proxy (skip), 3=guide (skip)
"""


IOP_POPULATE_OLD = """                meshPropsCastsShadows    = matched->castsShadows;
                meshPropsReceivesShadows = matched->receivesShadows;
                meshPropsHasEntry        = true;
"""

IOP_POPULATE_NEW = """                meshPropsCastsShadows    = matched->castsShadows;
                meshPropsReceivesShadows = matched->receivesShadows;
                meshPropsDoubleSided     = matched->doubleSided;
                meshPropsOrientation     = matched->orientation;
                meshPropsPurpose         = matched->purpose;
                meshPropsHasEntry        = true;
"""


# (c) Skip mesh on purpose=proxy/guide. Anchor on the trailing end of
# the matched-log block so we only skip AFTER the log shows us what we
# ALMOST rendered.

IOP_PURPOSE_OLD = """            } else if (!meshReg.empty()) {
                SLOG("SpectralRender: mesh '%s' (leaf '%s') - no matching "
                     "SpectralMeshProperties entry (registry size=%zu)\\n",
                     meshPath.c_str(), meshLeaf.c_str(), meshReg.size());
            }
        }
"""

IOP_PURPOSE_NEW = """            } else if (!meshReg.empty()) {
                SLOG("SpectralRender: mesh '%s' (leaf '%s') - no matching "
                     "SpectralMeshProperties entry (registry size=%zu)\\n",
                     meshPath.c_str(), meshLeaf.c_str(), meshReg.size());
            }
        }

        // Purpose filtering: 2=proxy / 3=guide are viewport-only. Skip
        // them from the ray-tracing scene. They still draw in the viewer
        // via Drafting / USD proxy paths.
        if (meshPropsHasEntry &&
            (meshPropsPurpose == 2 || meshPropsPurpose == 3)) {
            SLOG("SpectralRender: skipping mesh '%s' (purpose=%s)\\n",
                 prim.GetPath().GetString().c_str(),
                 (meshPropsPurpose == 2) ? "proxy" : "guide");
            continue;
        }
"""


# (d) XOR orientation into the existing flipNormals pass. The existing
# block guards with (meshPropsHasEntry && meshFlipNormals); we widen it
# to include orientation flipping as an equivalent trigger.

IOP_FLIP_OLD = """            // SpectralMeshProperties flip-normals: negate everything
            // (shading normals AND face normal) so backface-culling and
            // geometric orientation stay consistent. Runs last so it
            // applies to whichever normals the previous steps produced.
            if (meshPropsHasEntry && meshFlipNormals) {
                for (auto& tri : data.triangles) {
                    tri.n0 = -tri.n0;
                    tri.n1 = -tri.n1;
                    tri.n2 = -tri.n2;
                    tri.faceNormal = -tri.faceNormal;
                }
            }
"""

IOP_FLIP_NEW = """            // SpectralMeshProperties flip-normals + orientation: both
            // negate everything. orientation=leftHanded is the USD-idiomatic
            // way to express the same concept as flipNormals; when either
            // is active we flip. When BOTH are active, they cancel (XOR).
            const bool effFlip = meshPropsHasEntry &&
                                 (meshFlipNormals ^ (meshPropsOrientation == 1));
            if (effFlip) {
                for (auto& tri : data.triangles) {
                    tri.n0 = -tri.n0;
                    tri.n1 = -tri.n1;
                    tri.n2 = -tri.n2;
                    tri.faceNormal = -tri.faceNormal;
                }
            }
"""


# (e) Stamp doubleSided onto all materials used by this mesh. Add this
# block just before `_scene->SetMeshData(...)` so we catch whatever
# material IDs the triangulation produced, regardless of source path.

IOP_STAMP_OLD = """            _scene->SetMeshData(data.id, std::move(data));
            meshCount++;
        }
"""

IOP_STAMP_NEW = """            // Stamp per-mesh doubleSided onto the materials this mesh
            // uses. Last-write-wins for shared materials; usually a non-
            // issue since each mesh typically gets its own material
            // variant after resolution. Harmless when doubleSided=true
            // (that's the default on SpectralMaterial too).
            if (meshPropsHasEntry && !meshPropsDoubleSided) {
                std::unordered_set<int> stampIds;
                for (const auto& tri : data.triangles) {
                    stampIds.insert(static_cast<int>(tri.materialId));
                }
                for (int mid : stampIds) {
                    if (mid >= 0 && mid < (int)_scene->GetMaterials().size()) {
                        _scene->GetMaterialMutable(mid).doubleSided = false;
                    }
                }
                SLOG("SpectralRender: mesh '%s' stamped doubleSided=false onto %zu materials\\n",
                     data.id.GetString().c_str(), stampIds.size());
            }

            _scene->SetMeshData(data.id, std::move(data));
            meshCount++;
        }
"""


# ============================================================================
#  SpectralScene.h / SpectralScene must expose GetMaterialMutable.
#  Check if it exists; if not add a minimal accessor.
#  (We'll handle this via a separate edit conditionally.)
#
#  Simpler path: const_cast the existing GetMaterials() result inside
#  the Iop stamp site. Avoids touching SpectralScene.h which isn't in
#  the 6-file scope above.
# ============================================================================

# Revise the stamp block to use const_cast so we don't need scene changes.

IOP_STAMP_NEW_V2 = """            // Stamp per-mesh doubleSided onto the materials this mesh
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

            _scene->SetMeshData(data.id, std::move(data));
            meshCount++;
        }
"""


# ============================================================================
#  SpectralIntegrator.cpp -- gate the normal flip on doubleSided
# ============================================================================

INT_OLD = """    // Ensure N faces the camera (flip if back-facing)
    float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
    if (NdotV < 0.f) N = -N;

    // Resolve textures at this hit point
    SpectralMaterial resolvedMat = _ResolveMaterial(mat, tri, w, uf, vf, scene);
"""

INT_NEW = """    // Ensure N faces the camera (flip if back-facing). If the material
    // is single-sided, a back-facing hit is treated as a miss: the ray
    // passes through unimpeded. Matches the GPU kernel and USD's
    // UsdGeomMesh.doubleSided semantics.
    float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
    if (NdotV < 0.f) {
        if (!mat.doubleSided) return 0.f;
        N = -N;
    }

    // Resolve textures at this hit point
    SpectralMaterial resolvedMat = _ResolveMaterial(mat, tri, w, uf, vf, scene);
"""


# ============================================================================
#  SpectralGPUParams.h -- add doubleSided to GPUMaterial
# ============================================================================

PARAMS_OLD = """    // Subsurface scattering: the kernel uses a wrap-diffuse approximation
    // because porting the CPU random walk (16 steps with ray-exit tests)
    // would thrash memory per-pixel. sssColor drives wavelength-dependent
    // scatter; sssRadius drives overall strength and wrap width.
    float3 sssColor;
    float  sssRadius;
};
"""

PARAMS_NEW = """    // Subsurface scattering: the kernel uses a wrap-diffuse approximation
    // because porting the CPU random walk (16 steps with ray-exit tests)
    // would thrash memory per-pixel. sssColor drives wavelength-dependent
    // scatter; sssRadius drives overall strength and wrap width.
    float3 sssColor;
    float  sssRadius;
    // Double-sided flag. When 0, back-facing primary hits are treated
    // as misses (ray passes through). Matches CPU and USD doubleSided.
    int    doubleSided;
};
"""


# ============================================================================
#  SpectralGPU.cpp -- two sub-edits:
#    (a) copy SpectralMaterial.doubleSided -> GPUMaterial.doubleSided
#        at upload time
#    (b) cover the field in the material checksum so toggling the knob
#        re-uploads the GPU materials (classic CLAUDE.md gotcha)
# ============================================================================

# (a) We anchor on a distinctive existing copy from SpectralMaterial to
# GPUMaterial. Looking for the block that sets sssColor+sssRadius on
# the GPU side, we add doubleSided there.

GPU_COPY1_OLD = """                gpuMats[i].sssColor        = make_float3(mats[i].sssColor[0],
                                                         mats[i].sssColor[1],
                                                         mats[i].sssColor[2]);
                gpuMats[i].sssRadius       = mats[i].sssRadius;
            }
            const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
"""

GPU_COPY1_NEW = """                gpuMats[i].sssColor        = make_float3(mats[i].sssColor[0],
                                                         mats[i].sssColor[1],
                                                         mats[i].sssColor[2]);
                gpuMats[i].sssRadius       = mats[i].sssRadius;
                gpuMats[i].doubleSided     = mats[i].doubleSided ? 1 : 0;
            }
            const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
"""


GPU_COPY2_OLD = """            gpuMats[i].sssColor        = make_float3(mats[i].sssColor[0],
                                                     mats[i].sssColor[1],
                                                     mats[i].sssColor[2]);
            gpuMats[i].sssRadius       = mats[i].sssRadius;
        }
        const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
"""

GPU_COPY2_NEW = """            gpuMats[i].sssColor        = make_float3(mats[i].sssColor[0],
                                                     mats[i].sssColor[1],
                                                     mats[i].sssColor[2]);
            gpuMats[i].sssRadius       = mats[i].sssRadius;
            gpuMats[i].doubleSided     = mats[i].doubleSided ? 1 : 0;
        }
        const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
"""


# (b) Extend material checksum. Anchor on the existing metalType hash.

GPU_CHK_OLD = """            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
        }
"""

GPU_CHK_NEW = """            matCheck ^= static_cast<unsigned int>(mats[i].metalType) * 7u;
            matCheck ^= (mats[i].doubleSided ? 1u : 0u) * 2246822519u;
        }
"""


# ============================================================================
#  SpectralGPUKernel.cu -- gate primary normal flip on doubleSided.
#  When single-sided and back-facing, jump to spectral_miss.
# ============================================================================

KERNEL_OLD = """                float3 V = normalize3(neg3(dir));
                if (dot3raw(N,V)<0.f) N=neg3(N);
"""

KERNEL_NEW = """                float3 V = normalize3(neg3(dir));
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
#  Edit definitions
# ============================================================================

EDITS_MAT = [
    ("SpectralMaterial.h: add doubleSided field",
     MAT_OLD, MAT_NEW,
     "bool    doubleSided = true;"),
]
EDITS_IOP = [
    ("Iop: add 3 meshProps locals",
     IOP_LOCALS_OLD, IOP_LOCALS_NEW,
     "bool    meshPropsDoubleSided     = true;"),
    ("Iop: populate the 3 locals from matched->",
     IOP_POPULATE_OLD, IOP_POPULATE_NEW,
     "meshPropsDoubleSided     = matched->doubleSided;"),
    ("Iop: skip mesh when purpose is proxy or guide",
     IOP_PURPOSE_OLD, IOP_PURPOSE_NEW,
     "Purpose filtering: 2=proxy / 3=guide"),
    ("Iop: XOR orientation into flipNormals application",
     IOP_FLIP_OLD, IOP_FLIP_NEW,
     "meshFlipNormals ^ (meshPropsOrientation == 1)"),
    ("Iop: stamp doubleSided onto per-mesh materials",
     IOP_STAMP_OLD, IOP_STAMP_NEW_V2,
     "stamped doubleSided=false onto"),
]
EDITS_INT = [
    ("SpectralIntegrator.cpp: gate normal flip on doubleSided",
     INT_OLD, INT_NEW,
     "if (!mat.doubleSided) return 0.f;"),
]
EDITS_PARAMS = [
    ("SpectralGPUParams.h: add doubleSided to GPUMaterial",
     PARAMS_OLD, PARAMS_NEW,
     "int    doubleSided;"),
]
EDITS_GPU_CPP = [
    ("SpectralGPU.cpp: copy doubleSided at first upload site",
     GPU_COPY1_OLD, GPU_COPY1_NEW,
     "                gpuMats[i].doubleSided     = mats[i].doubleSided ? 1 : 0;"),
    ("SpectralGPU.cpp: copy doubleSided at second upload site",
     GPU_COPY2_OLD, GPU_COPY2_NEW,
     "            gpuMats[i].doubleSided     = mats[i].doubleSided ? 1 : 0;\n        }"),
    ("SpectralGPU.cpp: checksum doubleSided for re-upload invalidation",
     GPU_CHK_OLD, GPU_CHK_NEW,
     "(mats[i].doubleSided ? 1u : 0u)"),
]
EDITS_KERNEL = [
    ("SpectralGPUKernel.cu: gate primary normal flip on doubleSided",
     KERNEL_OLD, KERNEL_NEW,
     "if (!mat.doubleSided) {"),
]


# ============================================================================
#  Apply scaffold (same as earlier patches)
# ============================================================================

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
    bak = ".bak_mpfields"
    ok = True
    ok &= process(args.src / "SpectralMaterial.h",   EDITS_MAT,    args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralRenderIop.cpp",EDITS_IOP,    args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralIntegrator.cpp",EDITS_INT,   args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPUParams.h",  EDITS_PARAMS, args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPU.cpp",      EDITS_GPU_CPP,args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPUKernel.cu", EDITS_KERNEL, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Then test on SpectralMeshProperties:")
    print("  - doubleSided knob toggles backface visibility per mesh")
    print("  - orientation = left-handed flips normals (like flip-normals)")
    print("  - purpose = proxy / guide skips the mesh from the render")


if __name__ == "__main__":
    main()
