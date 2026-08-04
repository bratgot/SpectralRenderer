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
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

bool SpectralMeshToVdb::Convert(const float* positions, size_t numVerts,
                                const uint32_t* indices, size_t numIndices,
                                float voxelSize, float halfBandVoxels,
                                int mode, const char* outPath, std::string& error,
                                const float* colors)
{
#ifndef SPECTRAL_HAS_VDB
    (void)positions; (void)numVerts; (void)indices; (void)numIndices;
    (void)voxelSize; (void)halfBandVoxels; (void)mode; (void)outPath;
    (void)colors;
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

        // Optional per-vertex colours -> a Vec3f "Cd" grid over the density
        // grid's active voxels. Vertex colours SPLAT into their voxels; each
        // active voxel takes the nearest splat via an expanding-shell search
        // (vertices sit on the surface, so band voxels find one quickly); a
        // coarse occupancy grid short-circuits deep fog-interior voxels to
        // the mesh's average colour instead of scanning empty shells.
        if (colors && numVerts > 0) {
            auto pack = [](const openvdb::Coord& c) -> long long {
                const long long bias = 1 << 20;
                return (((long long)(c.x() + bias) & 0x1FFFFF) << 42) |
                       (((long long)(c.y() + bias) & 0x1FFFFF) << 21) |
                       (((long long)(c.z() + bias) & 0x1FFFFF));
            };
            struct Acc { openvdb::Vec3d c{0, 0, 0}; int n = 0; };
            std::unordered_map<long long, Acc> splat;
            std::unordered_set<long long> coarse;   // splat occupancy >> 3
            splat.reserve(numVerts * 2);
            openvdb::Vec3d gAvg(0, 0, 0);
            for (size_t i = 0; i < numVerts; ++i) {
                const openvdb::Vec3d ws(positions[i * 3 + 0], positions[i * 3 + 1],
                                        positions[i * 3 + 2]);
                const openvdb::Vec3d col(colors[i * 3 + 0], colors[i * 3 + 1],
                                         colors[i * 3 + 2]);
                const openvdb::Coord c = xform->worldToIndexCellCentered(ws);
                Acc& a = splat[pack(c)];
                a.c += col; a.n += 1;
                coarse.insert(pack(openvdb::Coord(c.x() >> 3, c.y() >> 3, c.z() >> 3)));
                gAvg += col;
            }
            gAvg /= double(numVerts);
            const openvdb::Vec3f avgF(float(gAvg.x()), float(gAvg.y()), float(gAvg.z()));
            auto cd = openvdb::Vec3fGrid::create(avgF);   // background = mesh average
            cd->setTransform(xform->copy());
            cd->setName("Cd");
            auto acc = cd->getAccessor();
            const int R = std::max(4, int(halfBand) + 2);
            for (auto it = grid->cbeginValueOn(); it; ++it) {
                const openvdb::Coord c = it.getCoord();
                // Coarse cull: any splat within +/-1 coarse cell (>= 8 voxels)?
                // (nearSplat, not `near`: windows.h defines near/far as macros.)
                bool nearSplat = false;
                const openvdb::Coord cc(c.x() >> 3, c.y() >> 3, c.z() >> 3);
                for (int dx = -1; dx <= 1 && !nearSplat; ++dx)
                    for (int dy = -1; dy <= 1 && !nearSplat; ++dy)
                        for (int dz = -1; dz <= 1 && !nearSplat; ++dz)
                            nearSplat = coarse.count(pack(openvdb::Coord(
                                cc.x() + dx, cc.y() + dy, cc.z() + dz))) != 0;
                openvdb::Vec3d best(gAvg);
                if (nearSplat) {
                    for (int r = 0; r <= R; ++r) {
                        openvdb::Vec3d sum(0, 0, 0); int n = 0;
                        for (int dx = -r; dx <= r; ++dx)
                            for (int dy = -r; dy <= r; ++dy)
                                for (int dz = -r; dz <= r; ++dz) {
                                    if (std::max(std::abs(dx),
                                            std::max(std::abs(dy), std::abs(dz))) != r)
                                        continue;   // shell surface only
                                    auto f = splat.find(pack(c.offsetBy(dx, dy, dz)));
                                    if (f != splat.end()) { sum += f->second.c; n += f->second.n; }
                                }
                        if (n) { best = sum / double(n); break; }
                    }
                }
                acc.setValue(c, openvdb::Vec3f(float(best.x()), float(best.y()),
                                               float(best.z())));
            }
            grids.push_back(cd);
        }

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
