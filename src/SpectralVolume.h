#pragma once
// SpectralVolume — volume data for spectral ray marching
// Stores sampled density/temperature/color grids for volume rendering.
// Created by Marten Blumen

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/bbox3d.h>
#include <vector>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

struct SpectralVolume {
    // Grid data (resampled to uniform 3D texture)
    std::vector<float> density;        // 3D density grid (NxNxN)
    std::vector<float> temperature;    // 3D temperature grid (optional)
    std::vector<float> flame;          // 3D flame grid (optional)
    std::vector<GfVec3f> color;        // 3D color grid (optional)
    int resX = 0, resY = 0, resZ = 0;

    // World-space bounds
    GfVec3f bboxMin = GfVec3f(0.f);
    GfVec3f bboxMax = GfVec3f(1.f);

    // Shading parameters
    float extinction      = 5.f;
    float scattering      = 3.f;
    float anisotropy      = 0.f;       // HG phase function g
    float densityMult     = 1.f;
    float emissionIntensity = 2.f;
    float tempMin         = 500.f;
    float tempMax         = 6500.f;
    float stepSize        = 0.f;       // 0 = auto
    float shadowStepMult  = 2.f;       // shadow steps are coarser
    float powderStrength  = 2.f;
    bool  jitter          = true;

    // Dual-lobe HG
    float gForward  = 0.65f;
    float gBackward = -0.25f;
    float lobeMix   = 0.70f;

    // Scatter colour
    GfVec3f scatterColor = GfVec3f(1.f);

    bool IsValid() const { return !density.empty() && resX > 0; }
    bool HasBbox() const { return resX > 0 && resY > 0 && resZ > 0; }

    // Sample density at normalised coordinates [0,1]^3
    float SampleDensity(float u, float v, float w) const {
        if (density.empty()) return 0.f;
        int ix = std::max(0, std::min(int(u * resX), resX - 1));
        int iy = std::max(0, std::min(int(v * resY), resY - 1));
        int iz = std::max(0, std::min(int(w * resZ), resZ - 1));
        return density[iz * resY * resX + iy * resX + ix] * densityMult;
    }

    // Sample temperature at normalised coordinates
    float SampleTemperature(float u, float v, float w) const {
        if (temperature.empty()) return 0.f;
        int ix = std::max(0, std::min(int(u * resX), resX - 1));
        int iy = std::max(0, std::min(int(v * resY), resY - 1));
        int iz = std::max(0, std::min(int(w * resZ), resZ - 1));
        return temperature[iz * resY * resX + iy * resX + ix];
    }

    // World position to normalised [0,1] coordinates
    GfVec3f WorldToNorm(const GfVec3f& p) const {
        GfVec3f size = bboxMax - bboxMin;
        return GfVec3f(
            (size[0] > 1e-6f) ? (p[0] - bboxMin[0]) / size[0] : 0.5f,
            (size[1] > 1e-6f) ? (p[1] - bboxMin[1]) / size[1] : 0.5f,
            (size[2] > 1e-6f) ? (p[2] - bboxMin[2]) / size[2] : 0.5f
        );
    }

    // Check if world position is inside volume
    bool Contains(const GfVec3f& p) const {
        return p[0] >= bboxMin[0] && p[0] <= bboxMax[0] &&
               p[1] >= bboxMin[1] && p[1] <= bboxMax[1] &&
               p[2] >= bboxMin[2] && p[2] <= bboxMax[2];
    }
};

PXR_NAMESPACE_CLOSE_SCOPE
