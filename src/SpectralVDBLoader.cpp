// SpectralVDBLoader — loads OpenVDB files into SpectralVolume
// Compiled as part of SpectralCore (links OpenVDB).
// SpectralRenderIop calls LoadVDB() without needing OpenVDB headers.
// Created by Marten Blumen

#include "SpectralVDBLoader.h"

#ifdef SPECTRAL_HAS_VDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>
#include <algorithm>
#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

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

            result.push_back({name, type, category});
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

    try {
        openvdb::initialize();
        openvdb::io::File file(filepath);
        file.open();

        auto vol = std::make_shared<SpectralVolume>();

        // Find grids by name (override) or auto-detect
        openvdb::FloatGrid::Ptr densityGrid;
        openvdb::FloatGrid::Ptr tempGrid;
        openvdb::FloatGrid::Ptr flameGrid;
        std::string wantDensity = (densityGridName && strlen(densityGridName) > 0) ? densityGridName : "";
        std::string wantTemp = (tempGridName && strlen(tempGridName) > 0) ? tempGridName : "";

        // Skip loading if density is explicitly "(none)"
        if (wantDensity == "(none)") wantDensity = "";

        // Find grids — only read the ones we need, skip the rest
        for (auto it = file.beginName(); it != file.endName(); ++it) {
            std::string name = *it;

            // Check if this grid name matches what we want BEFORE reading it
            bool isDensity = false, isTemp = false, isFlame = false;
            if (!densityGrid) {
                if (!wantDensity.empty() && name == wantDensity) isDensity = true;
                else if (wantDensity.empty() && _matchName(name, kDensityNames)) isDensity = true;
            }
            if (!tempGrid) {
                if (!wantTemp.empty() && name == wantTemp) isTemp = true;
                else if (wantTemp.empty() && _matchName(name, kTempNames)) isTemp = true;
            }
            if (!flameGrid && _matchName(name, kFlameNames)) isFlame = true;

            // Only read grid from disk if we actually need it
            if (isDensity || isTemp || isFlame || (!densityGrid && wantDensity.empty())) {
                auto base = file.readGrid(name);
                auto fg = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
                if (!fg) continue;

                if (isDensity) densityGrid = fg;
                else if (isTemp) tempGrid = fg;
                else if (isFlame) flameGrid = fg;
                else if (!densityGrid) densityGrid = fg; // first float as fallback
            }
        }

        file.close();

        if (!densityGrid) {
            fprintf(stderr, "SpectralVDB: no float grid found in %s\n", filepath);
            return nullptr;
        }

        // Get world-space bounding box
        auto bbox = densityGrid->evalActiveVoxelBoundingBox();
        auto wMin = densityGrid->indexToWorld(bbox.min());
        auto wMax = densityGrid->indexToWorld(bbox.max());
        vol->bboxMin = GfVec3f(float(wMin.x()), float(wMin.y()), float(wMin.z()));
        vol->bboxMax = GfVec3f(float(wMax.x()), float(wMax.y()), float(wMax.z()));

        // Resample to uniform grid
        auto dim = bbox.dim();
        float maxDim = float(std::max({dim.x(), dim.y(), dim.z()}));
        if (maxDim < 1) maxDim = 1;

        // maxRes <= 0 means "native" — use actual VDB dimensions (capped at 1024)
        int effectiveMax;
        if (maxRes <= 0) {
            effectiveMax = std::min(1024, int(maxDim));
        } else {
            effectiveMax = maxRes;
        }
        vol->resX = std::max(1, std::min(effectiveMax, int(dim.x() * effectiveMax / maxDim)));
        vol->resY = std::max(1, std::min(effectiveMax, int(dim.y() * effectiveMax / maxDim)));
        vol->resZ = std::max(1, std::min(effectiveMax, int(dim.z() * effectiveMax / maxDim)));

        fprintf(stderr, "SpectralVDB: native VDB dims = %dx%dx%d, resampling to %dx%dx%d (maxRes=%d)\n",
                dim.x(), dim.y(), dim.z(), vol->resX, vol->resY, vol->resZ, effectiveMax);

        size_t totalVoxels = size_t(vol->resX) * vol->resY * vol->resZ;
        vol->density.resize(totalVoxels, 0.f);

        // Sample density grid into uniform buffer
        openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> sampler(*densityGrid);

        for (int iz = 0; iz < vol->resZ; ++iz) {
            for (int iy = 0; iy < vol->resY; ++iy) {
                for (int ix = 0; ix < vol->resX; ++ix) {
                    float u = float(ix) / float(vol->resX);
                    float v = float(iy) / float(vol->resY);
                    float w = float(iz) / float(vol->resZ);
                    openvdb::Vec3d worldPos(
                        wMin.x() + u * (wMax.x() - wMin.x()),
                        wMin.y() + v * (wMax.y() - wMin.y()),
                        wMin.z() + w * (wMax.z() - wMin.z()));
                    openvdb::Vec3d idxPos = densityGrid->worldToIndex(worldPos);
                    float val = sampler.isSample(idxPos);
                    vol->density[iz * vol->resY * vol->resX + iy * vol->resX + ix] = val;
                }
            }
        }

        // Sample temperature grid if present
        if (tempGrid) {
            vol->temperature.resize(totalVoxels, 0.f);
            openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> tSampler(*tempGrid);
            for (int iz = 0; iz < vol->resZ; ++iz) {
                for (int iy = 0; iy < vol->resY; ++iy) {
                    for (int ix = 0; ix < vol->resX; ++ix) {
                        float u = float(ix) / float(vol->resX);
                        float v = float(iy) / float(vol->resY);
                        float w = float(iz) / float(vol->resZ);
                        openvdb::Vec3d worldPos(
                            wMin.x() + u * (wMax.x() - wMin.x()),
                            wMin.y() + v * (wMax.y() - wMin.y()),
                            wMin.z() + w * (wMax.z() - wMin.z()));
                        openvdb::Vec3d idxPos = tempGrid->worldToIndex(worldPos);
                        vol->temperature[iz * vol->resY * vol->resX + iy * vol->resX + ix] = tSampler.isSample(idxPos);
                    }
                }
            }
        }

        // Sample flame grid if present
        if (flameGrid) {
            vol->flame.resize(totalVoxels, 0.f);
            openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> fSampler(*flameGrid);
            for (int iz = 0; iz < vol->resZ; ++iz) {
                for (int iy = 0; iy < vol->resY; ++iy) {
                    for (int ix = 0; ix < vol->resX; ++ix) {
                        float u = float(ix) / float(vol->resX);
                        float v = float(iy) / float(vol->resY);
                        float w = float(iz) / float(vol->resZ);
                        openvdb::Vec3d worldPos(
                            wMin.x() + u * (wMax.x() - wMin.x()),
                            wMin.y() + v * (wMax.y() - wMin.y()),
                            wMin.z() + w * (wMax.z() - wMin.z()));
                        openvdb::Vec3d idxPos = flameGrid->worldToIndex(worldPos);
                        vol->flame[iz * vol->resY * vol->resX + iy * vol->resX + ix] = fSampler.isSample(idxPos);
                    }
                }
            }
        }

        fprintf(stderr, "SpectralVDB: loaded '%s' — %dx%dx%d (bbox %.1f,%.1f,%.1f → %.1f,%.1f,%.1f)%s%s\n",
                filepath, vol->resX, vol->resY, vol->resZ,
                vol->bboxMin[0], vol->bboxMin[1], vol->bboxMin[2],
                vol->bboxMax[0], vol->bboxMax[1], vol->bboxMax[2],
                tempGrid ? " +temperature" : "",
                flameGrid ? " +flame" : "");

        return vol;
    } catch (const std::exception& e) {
        fprintf(stderr, "SpectralVDB: error loading '%s': %s\n", filepath, e.what());
        return nullptr;
    }
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
