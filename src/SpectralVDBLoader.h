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
    };

    /// Scan a VDB file and return info about all grids.
    /// Caller does NOT need OpenVDB headers.
    static std::vector<GridInfo> DiscoverGrids(const char* filepath);

    /// Load a VDB file and return a SpectralVolume.
    /// densityGrid/tempGrid: override grid names (empty = auto-detect).
    static std::shared_ptr<SpectralVolume> Load(const char* filepath,
        const char* densityGrid = nullptr,
        const char* tempGrid = nullptr);
};

PXR_NAMESPACE_CLOSE_SCOPE
