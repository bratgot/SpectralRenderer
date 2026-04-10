// SpectralVDBLoader — loads OpenVDB files into SpectralVolume
// Compiled as part of SpectralCore (links OpenVDB).
// SpectralRenderIop calls LoadVDB() without needing OpenVDB headers.
// Created by Marten Blumen

#include "SpectralVDBLoader.h"

#ifdef SPECTRAL_HAS_VDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/Dense.h>
#include <nanovdb/NanoVDB.h>
#include <nanovdb/tools/CreateNanoGrid.h>

// OpenVDB 12 compat shim: NanoVDB's NodeAccessor<GridT> calls gridClass() and map()
// which don't exist on openvdb::FloatGrid. Add them via thin derived class.
struct FloatGridCompat : public openvdb::FloatGrid {
    nanovdb::GridClass gridClass() const {
        switch (this->getGridClass()) {
            case openvdb::GRID_LEVEL_SET:  return nanovdb::GridClass::LevelSet;
            case openvdb::GRID_FOG_VOLUME: return nanovdb::GridClass::FogVolume;
            case openvdb::GRID_STAGGERED:  return nanovdb::GridClass::Staggered;
            default:                       return nanovdb::GridClass::Unknown;
        }
    }
    nanovdb::Map map() const {
        auto vs = this->voxelSize();
        auto origin = this->transform().indexToWorld(openvdb::Vec3d(0, 0, 0));
        return nanovdb::Map(vs[0], nanovdb::Vec3d(origin.x(), origin.y(), origin.z()));
    }
};
static inline nanovdb::GridHandle<> createNanoFromOpenVDB(const openvdb::FloatGrid& grid) {
    return nanovdb::tools::createNanoGrid<FloatGridCompat>(
        reinterpret_cast<const FloatGridCompat&>(grid));
}
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>

PXR_NAMESPACE_OPEN_SCOPE

// --- LRU Cache statics (function-local to avoid init order issues) ---
SpectralVDBLoader::CacheList& SpectralVDBLoader::_cacheList() { static CacheList s; return s; }
SpectralVDBLoader::CacheMap& SpectralVDBLoader::_cacheMap() { static CacheMap s; return s; }
std::mutex& SpectralVDBLoader::_cacheMutex() { static std::mutex s; return s; }
int& SpectralVDBLoader::_maxCacheEntries() { static int s = 64; return s; }
int& SpectralVDBLoader::_cacheHits() { static int s = 0; return s; }
int& SpectralVDBLoader::_cacheMisses() { static int s = 0; return s; }

void SpectralVDBLoader::ClearCache() {
    std::lock_guard<std::mutex> lock(_cacheMutex());
    _cacheList().clear();
    _cacheMap().clear();
    _cacheHits() = 0;
    _cacheMisses() = 0;
}

void SpectralVDBLoader::SetCacheSize(int maxEntries) {
    std::lock_guard<std::mutex> lock(_cacheMutex());
    _maxCacheEntries() = std::max(1, maxEntries);
    // Evict if over new limit
    while ((int)_cacheList().size() > _maxCacheEntries()) {
        auto& back = _cacheList().back();
        _cacheMap().erase(back.first);
        _cacheList().pop_back();
    }
}

void SpectralVDBLoader::GetCacheStats(int& hits, int& misses, int& entries) {
    std::lock_guard<std::mutex> lock(_cacheMutex());
    hits = _cacheHits();
    misses = _cacheMisses();
    entries = (int)_cacheList().size();
}

// Grid name matching tables (same heuristics as VDBmarcher)
static bool _matchName(const std::string& n, const char** list) {
    for (int i = 0; list[i]; ++i) if (n == list[i]) return true;
    return false;
}
static const char* kDensityNames[] = {"density","density_1","smoke","soot","absorption","scatter",nullptr};
static const char* kTempNames[]    = {"temperature","heat","temp",nullptr};
static const char* kFlameNames[]   = {"flame","flames","fire","fuel","burn","incandescence","emission",nullptr};
static const char* kVelNames[]     = {"vel","v","velocity","motion",nullptr};
static const char* kColorNames[]   = {"Cd","color","colour","rgb","albedo",nullptr};

std::vector<SpectralVDBLoader::GridInfo> SpectralVDBLoader::DiscoverGrids(const char* filepath)
{
    std::vector<GridInfo> result;
    if (!filepath || strlen(filepath) == 0) return result;

    try {
        openvdb::initialize();
        openvdb::io::File file(filepath);
        file.open();

        for (auto it = file.beginName(); it != file.endName(); ++it) {
            std::string name = *it;
            auto meta = file.readGridMetadata(name);
            std::string type = meta->valueType();

            std::string category = "other";
            if (_matchName(name, kDensityNames) && type == "float") category = "density";
            else if (_matchName(name, kTempNames) && type == "float") category = "temperature";
            else if (_matchName(name, kFlameNames) && type == "float") category = "flame";
            else if (_matchName(name, kVelNames) && (type == "vec3s" || type == "vec3f")) category = "velocity";
            else if (_matchName(name, kColorNames) && (type == "vec3s" || type == "vec3f")) category = "color";
            else if (type == "float" && category == "other") category = "float";

            GridInfo gi;
            gi.name = name;
            gi.type = type;
            gi.category = category;
            gi.voxelCount = meta->activeVoxelCount();
            result.push_back(gi);
        }

        file.close();
    } catch (const std::exception& e) {
        fprintf(stderr, "SpectralVDB: discover error: %s\n", e.what());
    }
    return result;
}

std::shared_ptr<SpectralVolume> SpectralVDBLoader::Load(const char* filepath,
    const char* densityGridName, const char* tempGridName, int maxRes)
{
    if (!filepath || strlen(filepath) == 0) return nullptr;

    CacheKey key;
    key.filepath = filepath;
    key.densityGrid = densityGridName ? densityGridName : "";
    key.tempGrid = tempGridName ? tempGridName : "";
    key.maxRes = maxRes;

    {
        std::lock_guard<std::mutex> lock(_cacheMutex());
        auto it = _cacheMap().find(key);
        if (it != _cacheMap().end()) {
            // Cache hit — move to front (most recently used)
            _cacheList().splice(_cacheList().begin(), _cacheList(), it->second);
            _cacheHits()++;
            return it->second->second;
        }
    }

    // Cache miss — load from disk
    auto vol = _LoadFromDisk(filepath, densityGridName, tempGridName, maxRes);
    if (!vol) {
        _cacheMisses()++;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(_cacheMutex());
        _cacheMisses()++;
        // Insert at front
        _cacheList().push_front({key, vol});
        _cacheMap()[key] = _cacheList().begin();
        // Evict oldest if over limit
        while ((int)_cacheList().size() > _maxCacheEntries()) {
            auto& back = _cacheList().back();
            _cacheMap().erase(back.first);
            _cacheList().pop_back();
        }
    }

    return vol;
}

std::shared_ptr<SpectralVolume> SpectralVDBLoader::_LoadFromDisk(const char* filepath,
    const char* densityGridName, const char* tempGridName, int maxRes)
{
    if (!filepath || strlen(filepath) == 0) return nullptr;

    // Preview loads (low res) skip temp/flame for speed
    bool densityOnly = (maxRes > 0 && maxRes <= 64);

    try {
        auto tStart = std::chrono::high_resolution_clock::now();

        openvdb::initialize();
        openvdb::io::File file(filepath);
        file.open();

        auto tOpen = std::chrono::high_resolution_clock::now();

        auto vol = std::make_shared<SpectralVolume>();

        openvdb::FloatGrid::Ptr densityGrid;
        openvdb::FloatGrid::Ptr tempGrid;
        openvdb::FloatGrid::Ptr flameGrid;
        std::string wantDensity = (densityGridName && strlen(densityGridName) > 0) ? densityGridName : "";
        std::string wantTemp = (tempGridName && strlen(tempGridName) > 0) ? tempGridName : "";
        if (wantDensity == "(none)") wantDensity = "";

        for (auto it = file.beginName(); it != file.endName(); ++it) {
            std::string name = *it;

            bool isDensity = false, isTemp = false, isFlame = false;
            if (!densityGrid) {
                if (!wantDensity.empty() && name == wantDensity) isDensity = true;
                else if (wantDensity.empty() && _matchName(name, kDensityNames)) isDensity = true;
            }
            if (!densityOnly) {
                if (!tempGrid) {
                    if (!wantTemp.empty() && name == wantTemp) isTemp = true;
                    else if (wantTemp.empty() && _matchName(name, kTempNames)) isTemp = true;
                }
                if (!flameGrid && _matchName(name, kFlameNames)) isFlame = true;
            }

            if (isDensity || isTemp || isFlame || (!densityGrid && wantDensity.empty())) {
                auto base = file.readGrid(name);
                auto fg = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
                if (!fg) continue;

                if (isDensity) densityGrid = fg;
                else if (isTemp) tempGrid = fg;
                else if (isFlame) flameGrid = fg;
                else if (!densityGrid) densityGrid = fg;
            }

            // Early exit once we have density (skip remaining grids for preview)
            if (densityOnly && densityGrid) break;
        }

        file.close();

        auto tGrids = std::chrono::high_resolution_clock::now();

        if (!densityGrid) {
            fprintf(stderr, "SpectralVDB: no float grid found in %s\n", filepath);
            return nullptr;
        }

        auto bbox = densityGrid->evalActiveVoxelBoundingBox();
        auto wMin = densityGrid->indexToWorld(bbox.min());
        auto wMax = densityGrid->indexToWorld(bbox.max());
        vol->bboxMin = GfVec3f(float(wMin.x()), float(wMin.y()), float(wMin.z()));
        vol->bboxMax = GfVec3f(float(wMax.x()), float(wMax.y()), float(wMax.z()));

        auto dim = bbox.dim();
        float maxDim = float(std::max({dim.x(), dim.y(), dim.z()}));
        if (maxDim < 1) maxDim = 1;

        int effectiveMax;
        if (maxRes <= 0) {
            effectiveMax = std::min(1024, int(maxDim));
        } else {
            effectiveMax = maxRes;
        }
        vol->resX = std::max(1, std::min(int(dim.x()), int(dim.x() * effectiveMax / maxDim)));
        vol->resY = std::max(1, std::min(int(dim.y()), int(dim.y() * effectiveMax / maxDim)));
        vol->resZ = std::max(1, std::min(int(dim.z()), int(dim.z() * effectiveMax / maxDim)));

        fprintf(stderr, "SpectralVDB: native VDB dims = %dx%dx%d, resampling to %dx%dx%d (maxRes=%d%s)\n",
                dim.x(), dim.y(), dim.z(), vol->resX, vol->resY, vol->resZ, effectiveMax,
                densityOnly ? " density-only" : "");

        size_t totalVoxels = size_t(vol->resX) * vol->resY * vol->resZ;
        vol->density.resize(totalVoxels, 0.f);

        int rX=vol->resX, rY=vol->resY, rZ=vol->resZ;
        double wMinX=wMin.x(), wMinY=wMin.y(), wMinZ=wMin.z();
        double wRangeX=wMax.x()-wMin.x(), wRangeY=wMax.y()-wMin.y(), wRangeZ=wMax.z()-wMin.z();

        if (densityOnly) {
            // ── FAST PREVIEW PATH ──────────────────────────────────────
            // Iterate active voxels directly (walks VDB tree efficiently,
            // skips empty space, no interpolation, no systematic traversal).
            // Maps VDB index coords → our uniform grid coords.
            size_t activeCount = densityGrid->activeVoxelCount();
            int stride = std::max(1, int(activeCount / (totalVoxels * 2)));
            auto bboxMin_idx = bbox.min();
            float dimX = std::max(1.f, float(dim.x()));
            float dimY = std::max(1.f, float(dim.y()));
            float dimZ = std::max(1.f, float(dim.z()));
            int count = 0;
            for (auto iter = densityGrid->cbeginValueOn(); iter; ++iter) {
                if (count++ % stride != 0) continue;
                auto coord = iter.getCoord();
                float u = float(coord.x() - bboxMin_idx.x()) / dimX;
                float v = float(coord.y() - bboxMin_idx.y()) / dimY;
                float w = float(coord.z() - bboxMin_idx.z()) / dimZ;
                int ix = std::max(0, std::min(rX-1, int(u * rX)));
                int iy = std::max(0, std::min(rY-1, int(v * rY)));
                int iz = std::max(0, std::min(rZ-1, int(w * rZ)));
                float val = iter.getValue();
                // Max-merge: multiple VDB voxels may map to same output cell
                float& dst = vol->density[iz * rY * rX + iy * rX + ix];
                if (val > dst) dst = val;
            }

        } else {
            // ── QUALITY RENDER PATH ────────────────────────────────────
            // Test NanoVDB conversion (timing only — kernel still uses tex3D)
            {
                auto tNano = std::chrono::high_resolution_clock::now();
                try {
                    auto handle = createNanoFromOpenVDB(*densityGrid);
                    auto tNanoDone = std::chrono::high_resolution_clock::now();
                    auto msNano = std::chrono::duration_cast<std::chrono::milliseconds>(tNanoDone - tNano).count();
                    fprintf(stderr, "SpectralVDB: NanoVDB test — density %.1f MB in %lldms (%zu active voxels)\n",
                            handle.size()/(1024.0*1024.0), msNano, densityGrid->activeVoxelCount());
                } catch (const std::exception& e) {
                    fprintf(stderr, "SpectralVDB: NanoVDB test failed: %s\n", e.what());
                }
            }

            // Dense path (active — kernel uses tex3D)
            {
                // Dense fallback
                vol->density.resize(totalVoxels, 0.f);
                bool isNativeRes = (vol->resX >= int(dim.x()) &&
                                    vol->resY >= int(dim.y()) &&
                                    vol->resZ >= int(dim.z()));

                if (isNativeRes) {
                    openvdb::tools::Dense<float, openvdb::tools::LayoutXYZ> denseGrid(bbox, vol->density.data());
                    openvdb::tools::copyToDense(*densityGrid, denseGrid);
                } else {
                    int nX = int(dim.x()), nY = int(dim.y()), nZ = int(dim.z());
                    size_t nativeVoxels = size_t(nX) * nY * nZ;
                    std::vector<float> nativeBuf(nativeVoxels, 0.f);
                    openvdb::tools::Dense<float, openvdb::tools::LayoutXYZ> denseNative(bbox, nativeBuf.data());
                    openvdb::tools::copyToDense(*densityGrid, denseNative);
                    float scaleX = float(nX) / float(rX);
                    float scaleY = float(nY) / float(rY);
                    float scaleZ = float(nZ) / float(rZ);
                    #pragma omp parallel for schedule(dynamic) if(totalVoxels > 10000)
                    for (int iz = 0; iz < rZ; ++iz) {
                        int sz = std::min(int(iz * scaleZ), nZ - 1);
                        for (int iy = 0; iy < rY; ++iy) {
                            int sy = std::min(int(iy * scaleY), nY - 1);
                            for (int ix = 0; ix < rX; ++ix) {
                                int sx = std::min(int(ix * scaleX), nX - 1);
                                vol->density[iz * rY * rX + iy * rX + ix] =
                                    nativeBuf[sz * nY * nX + sy * nX + sx];
                            }
                        }
                    }
                }

                // Resample temperature
                if (tempGrid) {
                    vol->temperature.resize(totalVoxels, 0.f);
                    bool isNR = (vol->resX >= int(dim.x()) && vol->resY >= int(dim.y()) && vol->resZ >= int(dim.z()));
                    if (isNR) {
                        openvdb::tools::Dense<float, openvdb::tools::LayoutXYZ> dg(bbox, vol->temperature.data());
                        openvdb::tools::copyToDense(*tempGrid, dg);
                    } else {
                        int nX=int(dim.x()),nY=int(dim.y()),nZ=int(dim.z());
                        std::vector<float> nb(size_t(nX)*nY*nZ,0.f);
                        openvdb::tools::Dense<float,openvdb::tools::LayoutXYZ> dn(bbox,nb.data());
                        openvdb::tools::copyToDense(*tempGrid,dn);
                        float sX=float(nX)/rX,sY=float(nY)/rY,sZ=float(nZ)/rZ;
                        #pragma omp parallel for schedule(dynamic) if(totalVoxels>10000)
                        for(int iz=0;iz<rZ;++iz){int sz=std::min(int(iz*sZ),nZ-1);
                            for(int iy=0;iy<rY;++iy){int sy=std::min(int(iy*sY),nY-1);
                                for(int ix=0;ix<rX;++ix){int sx=std::min(int(ix*sX),nX-1);
                                    vol->temperature[iz*rY*rX+iy*rX+ix]=nb[sz*nY*nX+sy*nX+sx];}}}
                    }
                }
                // Resample flame
                if (flameGrid) {
                    vol->flame.resize(totalVoxels, 0.f);
                    bool isNR = (vol->resX >= int(dim.x()) && vol->resY >= int(dim.y()) && vol->resZ >= int(dim.z()));
                    if (isNR) {
                        openvdb::tools::Dense<float,openvdb::tools::LayoutXYZ> dg(bbox,vol->flame.data());
                        openvdb::tools::copyToDense(*flameGrid,dg);
                    } else {
                        int nX=int(dim.x()),nY=int(dim.y()),nZ=int(dim.z());
                        std::vector<float> nb(size_t(nX)*nY*nZ,0.f);
                        openvdb::tools::Dense<float,openvdb::tools::LayoutXYZ> dn(bbox,nb.data());
                        openvdb::tools::copyToDense(*flameGrid,dn);
                        float sX=float(nX)/rX,sY=float(nY)/rY,sZ=float(nZ)/rZ;
                        #pragma omp parallel for schedule(dynamic) if(totalVoxels>10000)
                        for(int iz=0;iz<rZ;++iz){int sz=std::min(int(iz*sZ),nZ-1);
                            for(int iy=0;iy<rY;++iy){int sy=std::min(int(iy*sY),nY-1);
                                for(int ix=0;ix<rX;++ix){int sx=std::min(int(ix*sX),nX-1);
                                    vol->flame[iz*rY*rX+iy*rX+ix]=nb[sz*nY*nX+sy*nX+sx];}}}
                    }
                }
            }
        }

        auto tResample = std::chrono::high_resolution_clock::now();
        auto msOpen     = std::chrono::duration_cast<std::chrono::milliseconds>(tOpen - tStart).count();
        auto msGrids    = std::chrono::duration_cast<std::chrono::milliseconds>(tGrids - tOpen).count();
        auto msResample = std::chrono::duration_cast<std::chrono::milliseconds>(tResample - tGrids).count();
        auto msTotal    = std::chrono::duration_cast<std::chrono::milliseconds>(tResample - tStart).count();

        bool isNative = (vol->resX >= int(dim.x()) && vol->resY >= int(dim.y()) && vol->resZ >= int(dim.z()));

        fprintf(stderr, "SpectralVDB: loaded '%s' — %dx%dx%d (open=%lldms grids=%lldms resample=%lldms total=%lldms) [%s]%s%s\n",
                filepath, vol->resX, vol->resY, vol->resZ,
                msOpen, msGrids, msResample, msTotal,
                densityOnly ? "streaming" : (vol->useNanoVDB ? "NanoVDB" : (isNative ? "copyToDense" : "copyToDense+downsample")),
                tempGrid && !densityOnly ? " +temperature" : "",
                flameGrid && !densityOnly ? " +flame" : "");

        return vol;
    } catch (const std::exception& e) {
        fprintf(stderr, "SpectralVDB: error loading '%s': %s\n", filepath, e.what());
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Prefetch — background thread loads neighboring frames into cache
// ---------------------------------------------------------------------------
static std::thread s_prefetchThread;
static std::atomic<bool> s_prefetchRunning{false};
static std::atomic<bool> s_prefetchCancel{false};

static std::string _resolvePattern(const std::string& pattern, int frame)
{
    std::string result = pattern;
    // Find longest run of '#' and replace with zero-padded frame number
    size_t hashStart = result.find('#');
    if (hashStart == std::string::npos) return result;
    size_t hashEnd = hashStart;
    while (hashEnd < result.size() && result[hashEnd] == '#') ++hashEnd;
    int pad = int(hashEnd - hashStart);
    char buf[32];
    snprintf(buf, sizeof(buf), "%0*d", pad, frame);
    result.replace(hashStart, hashEnd - hashStart, buf);
    return result;
}

void SpectralVDBLoader::PrefetchNeighbors(const char* filepath,
    const char* densityGrid, const char* tempGrid,
    int currentFrame, int maxRes,
    const std::string& framePattern, int firstFrame, int lastFrame)
{
    // Cancel any running prefetch
    s_prefetchCancel.store(true);
    if (s_prefetchThread.joinable()) {
        s_prefetchThread.join();
    }
    s_prefetchCancel.store(false);

    // Don't prefetch if no pattern
    if (framePattern.empty() || framePattern.find('#') == std::string::npos) return;

    std::string pattern = framePattern;
    std::string dGrid = densityGrid ? densityGrid : "";
    std::string tGrid = tempGrid ? tempGrid : "";

    s_prefetchRunning.store(true);
    s_prefetchThread = std::thread([=]() {
        // Prefetch N+1, N-1, N+2, N-2, N+3, N-3 (interleaved for scrub in either direction)
        int offsets[] = {1, -1, 2, -2, 3, -3, 4, -4};
        for (int off : offsets) {
            if (s_prefetchCancel.load()) break;
            int f = currentFrame + off;
            if (f < firstFrame || f > lastFrame) continue;

            std::string resolved = _resolvePattern(pattern, f);

            // Check if already cached
            CacheKey key;
            key.filepath = resolved;
            key.densityGrid = dGrid;
            key.tempGrid = tGrid;
            key.maxRes = maxRes;
            {
                std::lock_guard<std::mutex> lock(_cacheMutex());
                if (_cacheMap().find(key) != _cacheMap().end()) continue; // already cached
            }

            // Load into cache via normal Load path
            Load(resolved.c_str(),
                 dGrid.empty() ? nullptr : dGrid.c_str(),
                 tGrid.empty() ? nullptr : tGrid.c_str(),
                 maxRes);
        }
        s_prefetchRunning.store(false);
    });
    s_prefetchThread.detach();
}

// ---------------------------------------------------------------------------
// LoadMetadataOnly — read only bbox from VDB file header (~1ms)
// ---------------------------------------------------------------------------
std::shared_ptr<SpectralVolume> SpectralVDBLoader::LoadMetadataOnly(
    const char* filepath, const char* densityGridName)
{
    if (!filepath || strlen(filepath) == 0) return nullptr;

    try {
        openvdb::initialize();
        openvdb::io::File file(filepath);
        file.open();

        auto vol = std::make_shared<SpectralVolume>();
        std::string wantDensity = (densityGridName && strlen(densityGridName) > 0) ? densityGridName : "";

        // Read only the density grid — skip all others
        for (auto it = file.beginName(); it != file.endName(); ++it) {
            std::string name = *it;

            bool match = false;
            if (!wantDensity.empty() && name == wantDensity) match = true;
            else if (wantDensity.empty() && _matchName(name, kDensityNames)) match = true;
            else if (wantDensity.empty()) match = true; // first float

            if (!match) continue;

            // Read just this one grid for its bbox
            auto base = file.readGrid(name);
            auto fg = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
            if (!fg) continue;

            auto bbox = fg->evalActiveVoxelBoundingBox();
            auto wMin = fg->indexToWorld(bbox.min());
            auto wMax = fg->indexToWorld(bbox.max());
            vol->bboxMin = GfVec3f(float(wMin.x()), float(wMin.y()), float(wMin.z()));
            vol->bboxMax = GfVec3f(float(wMax.x()), float(wMax.y()), float(wMax.z()));
            auto dim = bbox.dim();
            vol->resX = dim.x(); vol->resY = dim.y(); vol->resZ = dim.z();
            // No density array — just bbox for viewport wireframe
            vol->density.clear();
            vol->temperature.clear();
            break;
        }

        file.close();
        return vol;
    } catch (const std::exception& e) {
        fprintf(stderr, "SpectralVDB: metadata error '%s': %s\n", filepath, e.what());
        return nullptr;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_VDB
