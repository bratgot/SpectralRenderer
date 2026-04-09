#pragma once
// SpectralVDBLoader — loads OpenVDB files into SpectralVolume
// Header has NO OpenVDB includes so callers don't need OpenVDB.
// Created by Marten Blumen

#include "SpectralVolume.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <mutex>
#include <thread>
#include <atomic>

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
    static std::vector<GridInfo> DiscoverGrids(const char* filepath);

    /// Load a VDB file and return a SpectralVolume.
    /// Uses LRU cache — repeated loads of same file+res are instant.
    static std::shared_ptr<SpectralVolume> Load(const char* filepath,
        const char* densityGrid = nullptr,
        const char* tempGrid = nullptr,
        int maxRes = 128);

    /// Read only VDB metadata (bbox, grid dimensions) without loading voxel data.
    static std::shared_ptr<SpectralVolume> LoadMetadataOnly(const char* filepath,
        const char* densityGrid = nullptr);

    /// Clear the entire cache (e.g. on file change or memory pressure)
    static void ClearCache();

    /// Set max cache entries (default 32)
    static void SetCacheSize(int maxEntries);

    /// Get cache stats
    static void GetCacheStats(int& hits, int& misses, int& entries);

    /// Prefetch frames around current frame in background thread
    /// Call this after loading current frame — worker loads neighbors into cache
    static void PrefetchNeighbors(const char* filepath,
        const char* densityGrid, const char* tempGrid,
        int currentFrame, int maxRes,
        const std::string& framePattern, int firstFrame, int lastFrame);

private:
    struct CacheKey {
        std::string filepath;
        std::string densityGrid;
        std::string tempGrid;
        int maxRes;
        bool operator==(const CacheKey& o) const {
            return filepath == o.filepath && densityGrid == o.densityGrid
                && tempGrid == o.tempGrid && maxRes == o.maxRes;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            size_t h = std::hash<std::string>()(k.filepath);
            h ^= std::hash<std::string>()(k.densityGrid) + 0x9e3779b9 + (h<<6) + (h>>2);
            h ^= std::hash<std::string>()(k.tempGrid) + 0x9e3779b9 + (h<<6) + (h>>2);
            h ^= std::hash<int>()(k.maxRes) + 0x9e3779b9 + (h<<6) + (h>>2);
            return h;
        }
    };

    using CacheList = std::list<std::pair<CacheKey, std::shared_ptr<SpectralVolume>>>;
    using CacheMap = std::unordered_map<CacheKey, CacheList::iterator, CacheKeyHash>;

    static CacheList& _cacheList();
    static CacheMap& _cacheMap();
    static std::mutex& _cacheMutex();
    static int& _maxCacheEntries();
    static int& _cacheHits();
    static int& _cacheMisses();

    /// Internal load without cache
    static std::shared_ptr<SpectralVolume> _LoadFromDisk(const char* filepath,
        const char* densityGrid, const char* tempGrid, int maxRes);
};

PXR_NAMESPACE_CLOSE_SCOPE
