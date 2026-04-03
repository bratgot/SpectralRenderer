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
            std::string name = it.gridName();
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
    const char* densityGridName, const char* tempGridName)
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
        std::string wantDensity = (densityGridName && strlen(densityGridName) > 0) ? densityGridName : "";
        std::string wantTemp = (tempGridName && strlen(tempGridName) > 0) ? tempGridName : "";

        for (auto it = file.beginName(); it != file.endName(); ++it) {
            auto base = file.readGrid(*it);
            auto fg = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
            if (!fg) continue;

            std::string name = *it;
            // Explicit name match
            if (!wantDensity.empty() && name == wantDensity) densityGrid = fg;
            if (!wantTemp.empty() && name == wantTemp) tempGrid = fg;

            // Auto-detect fallbacks
            if (!densityGrid && wantDensity.empty()) {
                if (_matchName(name, kDensityNames)) densityGrid = fg;
            }
            if (!tempGrid && wantTemp.empty()) {
                if (_matchName(name, kTempNames)) tempGrid = fg;
            }
            // Last resort: first float grid as density
            if (!densityGrid && wantDensity.empty()) densityGrid = fg;
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

        // Resample to uniform grid (max 128^3)
        int maxRes = 128;
        auto dim = bbox.dim();
        float maxDim = float(std::max({dim.x(), dim.y(), dim.z()}));
        if (maxDim < 1) maxDim = 1;
        vol->resX = std::max(1, std::min(maxRes, int(dim.x() * maxRes / maxDim)));
        vol->resY = std::max(1, std::min(maxRes, int(dim.y() * maxRes / maxDim)));
        vol->resZ = std::max(1, std::min(maxRes, int(dim.z() * maxRes / maxDim)));

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

        fprintf(stderr, "SpectralVDB: loaded '%s' — %dx%dx%d (bbox %.1f,%.1f,%.1f → %.1f,%.1f,%.1f)%s\n",
                filepath, vol->resX, vol->resY, vol->resZ,
                vol->bboxMin[0], vol->bboxMin[1], vol->bboxMin[2],
                vol->bboxMax[0], vol->bboxMax[1], vol->bboxMax[2],
                tempGrid ? " +temperature" : "");

        return vol;
    } catch (const std::exception& e) {
        fprintf(stderr, "SpectralVDB: error loading '%s': %s\n", filepath, e.what());
        return nullptr;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_VDB
