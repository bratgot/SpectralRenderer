// SpectralMeshToVdb -- triangle mesh -> VDB volume file.
// Compiled as part of SpectralCore (links OpenVDB); callers stay OpenVDB-free.
// Created by Marten Blumen

#include "pxr/pxr.h"
#include "SpectralMeshToVdb.h"

#ifdef SPECTRAL_HAS_VDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/MeshToVolume.h>
#include <openvdb/tools/LevelSetUtil.h>
#endif

#include <algorithm>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

bool SpectralMeshToVdb::Convert(const float* positions, size_t numVerts,
                                const uint32_t* indices, size_t numIndices,
                                float voxelSize, float halfBandVoxels,
                                int mode, const char* outPath, std::string& error)
{
#ifndef SPECTRAL_HAS_VDB
    (void)positions; (void)numVerts; (void)indices; (void)numIndices;
    (void)voxelSize; (void)halfBandVoxels; (void)mode; (void)outPath;
    error = "built without OpenVDB (SPECTRAL_HAS_VDB off)";
    return false;
#else
    if (!positions || numVerts < 3 || !indices || numIndices < 3) {
        error = "empty mesh";
        return false;
    }
    if (!(voxelSize > 1e-6f)) {
        error = "voxel size must be positive";
        return false;
    }
    const float halfBand = std::max(1.0f, halfBandVoxels);

    try {
        openvdb::initialize();

        std::vector<openvdb::Vec3s> pts(numVerts);
        for (size_t i = 0; i < numVerts; ++i)
            pts[i] = openvdb::Vec3s(positions[i * 3 + 0],
                                    positions[i * 3 + 1],
                                    positions[i * 3 + 2]);
        const size_t triCount = numIndices / 3;
        std::vector<openvdb::Vec3I> tris(triCount);
        for (size_t t = 0; t < triCount; ++t)
            tris[t] = openvdb::Vec3I(indices[t * 3 + 0],
                                     indices[t * 3 + 1],
                                     indices[t * 3 + 2]);

        openvdb::math::Transform::Ptr xform =
            openvdb::math::Transform::createLinearTransform(double(voxelSize));

        openvdb::FloatGrid::Ptr grid;
        if (mode == Shell) {
            // Open / non-watertight meshes: unsigned distance shell. Density
            // ramps 1 at the surface -> 0 at the band edge.
            grid = openvdb::tools::meshToUnsignedDistanceField<openvdb::FloatGrid>(
                *xform, pts, tris, std::vector<openvdb::Vec4I>(), halfBand);
            const float bandWorld = halfBand * voxelSize;
            openvdb::tools::foreach(grid->beginValueOn(),
                [bandWorld](const openvdb::FloatGrid::ValueOnIter& it) {
                    const float d = it.getValue();
                    it.setValue(std::max(0.f, 1.f - d / bandWorld));
                });
            grid->setGridClass(openvdb::GRID_FOG_VOLUME);
            grid->setName("density");
        } else if (mode == Fog) {
            // FULL interior bandwidth: a plain narrow-band meshToLevelSet turns
            // into a HOLLOW 1-voxel fog shell after sdfToFogVolume + dense copy
            // (the interior stays background) -- the baked smoke was invisible.
            // meshToSignedDistanceField with a huge interior band keeps every
            // inside voxel active, so the fog body is solid.
            grid = openvdb::tools::meshToSignedDistanceField<openvdb::FloatGrid>(
                *xform, pts, tris, std::vector<openvdb::Vec4I>(),
                halfBand, 1e6f);
            openvdb::tools::sdfToFogVolume(*grid);
            grid->setName("density");
        } else {
            grid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
                *xform, pts, tris, halfBand);
            grid->setName("surface");
        }

        if (!grid || grid->empty()) {
            error = "conversion produced an empty grid (voxel size too large?)";
            return false;
        }

        openvdb::io::File file(outPath);
        openvdb::GridPtrVec grids;
        grids.push_back(grid);
        file.write(grids);
        file.close();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
#endif
}

PXR_NAMESPACE_CLOSE_SCOPE
