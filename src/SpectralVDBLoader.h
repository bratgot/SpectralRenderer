#pragma once
// SpectralVDBLoader — loads OpenVDB files into SpectralVolume
// Header has NO OpenVDB includes so callers don't need OpenVDB.
// Created by Marten Blumen

#include "SpectralVolume.h"
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralVDBLoader {
public:
    /// Grid info from VDB file scan
    struct GridInfo {
        std::string name;
        std::string type;     // "float", "vec3s", "PointDataGrid", etc.
        std::string category; // "density", "temperature", "flame", "velocity", "color", "other"
        size_t voxelCount = 0;
    };

    /// Scan a VDB file and return info about all grids.
    /// Caller does NOT need OpenVDB headers.
    static std::vector<GridInfo> DiscoverGrids(const char* filepath);

    /// Load a VDB file and return a SpectralVolume.
    /// densityGrid/tempGrid: override grid names (empty = auto-detect).
    /// maxRes: maximum resolution per axis (128 for render, 64 for viewport preview).
    static std::shared_ptr<SpectralVolume> Load(const char* filepath,
        const char* densityGrid = nullptr,
        const char* tempGrid = nullptr,
        int maxRes = 128);

    /// Read only VDB metadata (bbox, grid dimensions) without loading voxel data.
    /// Returns a SpectralVolume with valid bbox but empty density/temperature arrays.
    /// ~1ms vs ~200ms for full Load. Use for viewport scrubbing.
    static std::shared_ptr<SpectralVolume> LoadMetadataOnly(const char* filepath,
        const char* densityGrid = nullptr);
};

PXR_NAMESPACE_CLOSE_SCOPE
