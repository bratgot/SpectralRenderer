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

    // World-space bounds (original, before transform)
    GfVec3f bboxMin = GfVec3f(0.f);
    GfVec3f bboxMax = GfVec3f(1.f);

    // Volume transform (applied on top of VDB world-space bounds)
    GfVec3f translate = GfVec3f(0.f);
    GfVec3f rotate    = GfVec3f(0.f);  // degrees XYZ
    GfVec3f scale     = GfVec3f(1.f);
    bool    hasTransform = false;

    // Cached transformed bbox (computed by BuildTransform)
    GfVec3f xfBboxMin = GfVec3f(0.f);
    GfVec3f xfBboxMax = GfVec3f(1.f);

    // Build transform from translate/rotate/scale. Call after setting those.
    void BuildTransform() {
        // Center of the original bbox
        GfVec3f center = (bboxMin + bboxMax) * 0.5f;
        GfVec3f halfSize = (bboxMax - bboxMin) * 0.5f;

        hasTransform = (translate.GetLength() > 1e-6f ||
                        rotate.GetLength() > 1e-6f ||
                        std::abs(scale[0]-1.f) > 1e-6f ||
                        std::abs(scale[1]-1.f) > 1e-6f ||
                        std::abs(scale[2]-1.f) > 1e-6f);

        // Compute axis-aligned bbox after scale+rotate+translate
        // For ray intersection we need the world-space AABB
        GfVec3f scaledHalf(halfSize[0]*scale[0], halfSize[1]*scale[1], halfSize[2]*scale[2]);

        // Rotation matrix (XYZ Euler)
        float cx=std::cos(rotate[0]*3.14159265f/180.f), sx=std::sin(rotate[0]*3.14159265f/180.f);
        float cy=std::cos(rotate[1]*3.14159265f/180.f), sy=std::sin(rotate[1]*3.14159265f/180.f);
        float cz=std::cos(rotate[2]*3.14159265f/180.f), sz=std::sin(rotate[2]*3.14159265f/180.f);
        // Row-major rotation matrix
        float m00=cy*cz, m01=sx*sy*cz-cx*sz, m02=cx*sy*cz+sx*sz;
        float m10=cy*sz, m11=sx*sy*sz+cx*cz, m12=cx*sy*sz-sx*cz;
        float m20=-sy,   m21=sx*cy,           m22=cx*cy;

        // Store rotation matrix for inverse transform
        _rotM[0]=m00; _rotM[1]=m01; _rotM[2]=m02;
        _rotM[3]=m10; _rotM[4]=m11; _rotM[5]=m12;
        _rotM[6]=m20; _rotM[7]=m21; _rotM[8]=m22;
        _center = center + translate;
        _invScale = GfVec3f(1.f/std::max(scale[0],1e-6f),
                            1.f/std::max(scale[1],1e-6f),
                            1.f/std::max(scale[2],1e-6f));

        // Expand AABB to enclose rotated+scaled box
        // Each corner contributes to min/max
        GfVec3f absR[3] = {
            GfVec3f(std::abs(m00),std::abs(m01),std::abs(m02)),
            GfVec3f(std::abs(m10),std::abs(m11),std::abs(m12)),
            GfVec3f(std::abs(m20),std::abs(m21),std::abs(m22))
        };
        GfVec3f newHalf(
            absR[0][0]*scaledHalf[0]+absR[0][1]*scaledHalf[1]+absR[0][2]*scaledHalf[2],
            absR[1][0]*scaledHalf[0]+absR[1][1]*scaledHalf[1]+absR[1][2]*scaledHalf[2],
            absR[2][0]*scaledHalf[0]+absR[2][1]*scaledHalf[1]+absR[2][2]*scaledHalf[2]);
        xfBboxMin = _center - newHalf;
        xfBboxMax = _center + newHalf;
    }

    // Inverse-transform world point to normalised [0,1]^3 volume coords
    GfVec3f InverseTransformPoint(const GfVec3f& p) const {
        GfVec3f local = p - _center;
        // Inverse rotation (transpose)
        GfVec3f unrot(
            _rotM[0]*local[0]+_rotM[3]*local[1]+_rotM[6]*local[2],
            _rotM[1]*local[0]+_rotM[4]*local[1]+_rotM[7]*local[2],
            _rotM[2]*local[0]+_rotM[5]*local[1]+_rotM[8]*local[2]);
        // Inverse scale
        GfVec3f unscaled(unrot[0]*_invScale[0], unrot[1]*_invScale[1], unrot[2]*_invScale[2]);
        // Back to original bbox center-relative, then to [0,1]
        GfVec3f origCenter = (bboxMin + bboxMax) * 0.5f;
        GfVec3f origHalf = (bboxMax - bboxMin) * 0.5f;
        GfVec3f orig = unscaled + origCenter;
        return GfVec3f(
            (origHalf[0]>1e-6f) ? (orig[0]-bboxMin[0])/(origHalf[0]*2.f) : 0.5f,
            (origHalf[1]>1e-6f) ? (orig[1]-bboxMin[1])/(origHalf[1]*2.f) : 0.5f,
            (origHalf[2]>1e-6f) ? (orig[2]-bboxMin[2])/(origHalf[2]*2.f) : 0.5f);
    }

    // Transform internals (public for GPU upload)
    float _rotM[9] = {1,0,0, 0,1,0, 0,0,1};
    GfVec3f _center = GfVec3f(0.f);
    GfVec3f _invScale = GfVec3f(1.f);

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
    float gradientMix    = 0.f;    // blend HG phase with density-gradient Lambertian
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

    // Phase 17: fire & explosions
    float flameOpacity     = 0.f;       // flame burns away density (0=none, 1=full)
    float flameTempMin     = 1200.f;    // flame grid min temperature (K)
    float flameTempMax     = 3500.f;    // flame grid max temperature (K)
    float coreGlow         = 0.f;       // dense core emission multiplier
    float coreTemp         = 4000.f;    // dense core temperature (K)
    bool  cherenkov        = false;     // Cherenkov blue glow
    float cherenkovStrength  = 1.f;     // Cherenkov intensity
    float cherenkovThreshold = 0.5f;    // density threshold for activation

    // Grid mixer
    float densityMix       = 1.f;     // fade density grid (0=off, 1=full)
    float tempMix          = 1.f;     // fade temperature grid
    float flameMix         = 1.f;     // fade flame grid

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
        // Out-of-bounds = outside actual volume (expanded AABB from rotation)
        if (u < 0.f || u > 1.f || v < 0.f || v > 1.f || w < 0.f || w > 1.f) return 0.f;
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
        if (u < 0.f || u > 1.f || v < 0.f || v > 1.f || w < 0.f || w > 1.f) return GfVec3f(0.f);
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

    // World position to normalised [0,1] coordinates (transform-aware)
    GfVec3f WorldToNorm(const GfVec3f& p) const {
        if (hasTransform) return InverseTransformPoint(p);
        GfVec3f size = bboxMax - bboxMin;
        return GfVec3f(
            (size[0] > 1e-6f) ? (p[0] - bboxMin[0]) / size[0] : 0.5f,
            (size[1] > 1e-6f) ? (p[1] - bboxMin[1]) / size[1] : 0.5f,
            (size[2] > 1e-6f) ? (p[2] - bboxMin[2]) / size[2] : 0.5f
        );
    }

    // Check if world position is inside volume (uses transformed AABB)
    bool Contains(const GfVec3f& p) const {
        GfVec3f mn = hasTransform ? xfBboxMin : bboxMin;
        GfVec3f mx = hasTransform ? xfBboxMax : bboxMax;
        return p[0] >= mn[0] && p[0] <= mx[0] &&
               p[1] >= mn[1] && p[1] <= mx[1] &&
               p[2] >= mn[2] && p[2] <= mx[2];
    }

    // Get effective bbox for ray-AABB intersection
    GfVec3f GetBboxMin() const { return hasTransform ? xfBboxMin : bboxMin; }
    GfVec3f GetBboxMax() const { return hasTransform ? xfBboxMax : bboxMax; }
};

PXR_NAMESPACE_CLOSE_SCOPE
