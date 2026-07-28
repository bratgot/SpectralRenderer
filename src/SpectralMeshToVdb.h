#pragma once
// SpectralMeshToVdb -- converts a triangle mesh to a VDB volume file.
// Method: OpenVDB tools::meshToLevelSet (narrow-band SDF rasterization, the
// same machinery Houdini's "VDB from Polygons" builds on), optionally
// followed by sdfToFogVolume for a density fog. Header has NO OpenVDB
// includes so callers don't need OpenVDB.
// Created by Marten Blumen

#include <cstddef>
#include <cstdint>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralMeshToVdb {
public:
    enum Mode {
        Fog = 0,        // narrow-band SDF -> fog density (1 inside, ramp at band)
        LevelSet = 1,   // signed distance field (surface shell rendering / tooling)
        Shell = 2       // unsigned distance -> density shell (open / soup meshes)
    };

    /// Convert an indexed triangle mesh (xyz float triples, uint32 indices,
    /// 3 per tri) into a .vdb written at outPath. World units; voxelSize in
    /// world units; halfBandVoxels = narrow-band half width in voxels
    /// (>= 1, typical 3). The density grid is named "density" (Fog/Shell)
    /// or "surface" (LevelSet) so the Volume node's default grid pick works.
    /// Returns false + error on failure.
    static bool Convert(const float* positions, size_t numVerts,
                        const uint32_t* indices, size_t numIndices,
                        float voxelSize, float halfBandVoxels,
                        int mode, const char* outPath, std::string& error);
};

PXR_NAMESPACE_CLOSE_SCOPE
