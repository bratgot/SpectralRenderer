#pragma once
// SpectralVDBLoader — loads OpenVDB files into SpectralVolume
// Header has NO OpenVDB includes so callers don't need OpenVDB.
// Created by Marten Blumen

#include "SpectralVolume.h"
#include <memory>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralVDBLoader {
public:
    /// Load a VDB file and return a SpectralVolume.
    /// Returns nullptr if file not found or no density grid.
    /// Caller does NOT need OpenVDB headers.
    static std::shared_ptr<SpectralVolume> Load(const char* filepath);
};

PXR_NAMESPACE_CLOSE_SCOPE
