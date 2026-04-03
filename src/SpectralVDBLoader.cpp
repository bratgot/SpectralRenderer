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

std::shared_ptr<SpectralVolume> SpectralVDBLoader::Load(const char* filepath)
{
    if (!filepath || strlen(filepath) == 0) return nullptr;

    try {
        openvdb::initialize();
        openvdb::io::File file(filepath);
        file.open();

        auto vol = std::make_shared<SpectralVolume>();

        // Find density grid (try "density" first, then first float grid)
        openvdb::FloatGrid::Ptr densityGrid;
        openvdb::FloatGrid::Ptr tempGrid;

        for (auto it = file.beginName(); it != file.endName(); ++it) {
            auto base = file.readGrid(*it);
            auto fg = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
            if (!fg) continue;

            if (*it == "density" && !densityGrid) {
                densityGrid = fg;
            } else if ((*it == "temperature" || *it == "temp") && !tempGrid) {
                tempGrid = fg;
            } else if (!densityGrid) {
                densityGrid = fg;  // fallback: first float grid
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
