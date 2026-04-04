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
    float flameIntensity  = 5.f;
    float stepSize        = 0.f;       // 0 = auto
    float shadowStepMult  = 2.f;       // shadow steps are coarser
    float powderStrength  = 2.f;
    bool  jitter          = true;

    // Dual-lobe HG
    float gForward  = 0.65f;
    float gBackward = -0.25f;
    float lobeMix   = 0.70f;

    // Phase function mode: 0=Dual-lobe HG, 1=Approximate Mie
    int   phaseMode      = 0;
    float mieDropletD    = 2.0f;     // microns

    // Rendering
    bool  spectralVolumes = false;   // true=spectral path, false=direct RGB

    // Scatter colour
    GfVec3f scatterColor = GfVec3f(1.f);

    // CIE blackbody emission (Phase 12)
    bool  useBlackbody   = true;

    // Chromatic extinction (Phase 12)
    bool  chromaticExtinction = false;
    float sigmaR = 1.0f;    // relative extinction at red
    float sigmaG = 1.0f;    // relative extinction at green
    float sigmaB = 1.2f;    // relative extinction at blue (higher = more blue scatter)

    // Shadow rays
    int   shadowSteps    = 8;       // shadow ray samples per light
    float shadowDensity  = 1.0f;    // multiplier on extinction for shadows

    // Quality
    float quality        = 5.0f;    // log step quality (1=fast, 10=final)
    bool  adaptiveStep   = true;    // larger steps in thin regions
    int   renderSamples  = 1;       // stochastic passes per pixel

    // Multiple scattering (Wrenninge 2015)
    bool  msApprox       = true;
    GfVec3f msTint       = GfVec3f(1.f, 0.97f, 0.95f);

    // Procedural detail noise (fBm)
    bool  noiseEnable    = false;
    bool  noiseNormalize = true;
    float noiseScale     = 4.0f;
    float noiseStrength  = 0.3f;
    int   noiseOctaves   = 3;
    float noiseRoughness = 0.5f;

    // Render mode: 0=Lit,1=Greyscale,2=Heat,3=Cool,4=Blackbody,5=Explosion
    int   renderMode     = 0;
    float intensity      = 1.0f;    // master brightness

    // Environment
    float envIntensity   = 1.0f;
    float envDiffuse     = 0.5f;

    bool IsValid() const { return !density.empty() && resX > 0; }
    bool HasBbox() const { return resX > 0 && resY > 0 && resZ > 0; }

    // Trilinear interpolation helper
    float _SampleTrilinear(const std::vector<float>& grid, float u, float v, float w) const {
        if (grid.empty()) return 0.f;
        float fx = u * (resX - 1), fy = v * (resY - 1), fz = w * (resZ - 1);
        int x0 = std::max(0, std::min(int(fx), resX - 2));
        int y0 = std::max(0, std::min(int(fy), resY - 2));
        int z0 = std::max(0, std::min(int(fz), resZ - 2));
        float dx = fx - x0, dy = fy - y0, dz = fz - z0;
        int x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
        int sX = 1, sY = resX, sZ = resY * resX;
        float c000 = grid[z0*sZ + y0*sY + x0*sX];
        float c100 = grid[z0*sZ + y0*sY + x1*sX];
        float c010 = grid[z0*sZ + y1*sY + x0*sX];
        float c110 = grid[z0*sZ + y1*sY + x1*sX];
        float c001 = grid[z1*sZ + y0*sY + x0*sX];
        float c101 = grid[z1*sZ + y0*sY + x1*sX];
        float c011 = grid[z1*sZ + y1*sY + x0*sX];
        float c111 = grid[z1*sZ + y1*sY + x1*sX];
        float c00 = c000*(1-dx) + c100*dx;
        float c10 = c010*(1-dx) + c110*dx;
        float c01 = c001*(1-dx) + c101*dx;
        float c11 = c011*(1-dx) + c111*dx;
        float c0 = c00*(1-dy) + c10*dy;
        float c1 = c01*(1-dy) + c11*dy;
        return c0*(1-dz) + c1*dz;
    }

    // Sample density at normalised coordinates [0,1]^3 — trilinear
    float SampleDensity(float u, float v, float w) const {
        return _SampleTrilinear(density, u, v, w) * densityMult;
    }

    // Sample temperature at normalised coordinates — trilinear
    float SampleTemperature(float u, float v, float w) const {
        return _SampleTrilinear(temperature, u, v, w);
    }

    // Sample flame at normalised coordinates — trilinear
    float SampleFlame(float u, float v, float w) const {
        return _SampleTrilinear(flame, u, v, w);
    }

    // Sample colour at normalised coordinates — trilinear
    GfVec3f SampleColor(float u, float v, float w) const {
        if (color.empty()) return GfVec3f(1.f);
        float fx = u * (resX - 1), fy = v * (resY - 1), fz = w * (resZ - 1);
        int x0 = std::max(0, std::min(int(fx), resX - 2));
        int y0 = std::max(0, std::min(int(fy), resY - 2));
        int z0 = std::max(0, std::min(int(fz), resZ - 2));
        float dx = fx - x0, dy = fy - y0, dz = fz - z0;
        int x1 = x0+1, y1 = y0+1, z1 = z0+1;
        int sY = resX, sZ = resY * resX;
        GfVec3f c000=color[z0*sZ+y0*sY+x0], c100=color[z0*sZ+y0*sY+x1];
        GfVec3f c010=color[z0*sZ+y1*sY+x0], c110=color[z0*sZ+y1*sY+x1];
        GfVec3f c001=color[z1*sZ+y0*sY+x0], c101=color[z1*sZ+y0*sY+x1];
        GfVec3f c011=color[z1*sZ+y1*sY+x0], c111=color[z1*sZ+y1*sY+x1];
        GfVec3f c00=c000*(1-dx)+c100*dx, c10=c010*(1-dx)+c110*dx;
        GfVec3f c01=c001*(1-dx)+c101*dx, c11=c011*(1-dx)+c111*dx;
        return (c00*(1-dy)+c10*dy)*(1-dz) + (c01*(1-dy)+c11*dy)*dz;
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
