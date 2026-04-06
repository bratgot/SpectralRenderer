#pragma once

// ---------------------------------------------------------------------------
// SpectralGPUParams
//
//   Shared struct between host (C++) and device (CUDA/OptiX).
//   Passed as the launch params to optixLaunch().
//   Must be plain data — no pointers to host objects, no vtables.
// ---------------------------------------------------------------------------

#ifdef __CUDACC__
  #include <cuda_runtime.h>
  #include <optix_device.h>
#endif

#ifndef __CUDACC__
  #include <optix_types.h>
  #include <cuda_runtime.h>
#endif

namespace spectral_gpu {

// GPU-side material
struct GPUMaterial {
    float3 baseColor;
    float  metallic;
    float  roughness;
    float  ior;
    float  opacity;
    float3 emissiveColor;
    float  abbeNumber;
    float  thinFilmThickness;
    int    baseColorTexId;   // -1 = no texture
    float  textureBlend;     // 0=base color, 1=full texture
    int    bumpMapTexId;     // -1 = no bump map
    float  bumpStrength;     // bump intensity
    float3 absorptionColor;  // volume color (white=clear)
    float  absorptionDensity; // 0=clear, higher=darker
    // Phase 14: diffraction grating
    float  gratingSpacing;   // um, 0=disabled
    float  gratingStrength;  // blend factor
    // Phase 14: fluorescence
    float  fluorAbsorb;      // absorption center wavelength (nm)
    float  fluorEmit;        // emission center wavelength (nm)
    float  fluorStrength;    // intensity, 0=disabled
};

// GPU-side texture (header only — pixel data is a separate device buffer)
struct GPUTexture {
    float* pixels;        // device pointer to RGBA float data
    int    width;
    int    height;
    int    channels;
};

// GPU-side light
struct GPULight {
    int    type;          // 0=distant, 1=sphere, 2=rect, 3=dome, 4=spot
    float3 position;
    float3 direction;
    float3 color;
    float  intensity;     // effective intensity (intensity * 2^exposure)
    float  colorTemperature;
    int    useColorTemp;  // 0=RGB, 1=blackbody
    float  radius;
    float  width;
    float  height;
    float  cosConeAngle;  // cos(halfAngle) for spot
    float  cosPenumbra;   // cos(innerHalfAngle) for spot
};

struct CameraParams {
    float projInverse[16];
    float viewToWorld[16];
    float fStop;            // 0 = pinhole
    float focusDistance;
    float focalLength;      // mm
    float right[3];         // camera right vector (world)
    float up[3];            // camera up vector (world)
    float forward[3];       // camera forward vector (world)
};

#define SPECTRAL_MAX_GPU_VOLUMES 8

struct GPUVolume {
    float*  density;          // device ptr
    float*  temperature;      // device ptr (null if none)
    float*  flame;            // device ptr (null if none)
    int     resX, resY, resZ;
    float3  bboxMin, bboxMax;
    float   extinction;
    float   scattering;
    float   densityMult;
    float   gForward;
    float   gBackward;
    float   lobeMix;
    float   emissionIntensity;
    float   tempMin, tempMax;
    float   powder;
    float3  scatterColor;
    float   stepSize;
    int     jitter;
    int     phaseMode;
    float   mieDropletD;
    int     shadowSteps;
    float   shadowDensity;
    float   quality;
    int     adaptiveStep;
    int     renderMode;
    float   intensity;
    float   flameIntensity;
    // Noise
    int     noiseEnable;
    float   noiseScale;
    float   noiseStrength;
    int     noiseOctaves;
    float   noiseRoughness;
    // Transform
    int     hasTransform;
    float3  xfCenter;
    float3  invScale;
    float   invRotM[9];
    float3  origBboxMin;
    float3  origBboxMax;
    // Phase 14: chromatic extinction
    int     chromaticExtinction;
    float   sigmaR, sigmaG, sigmaB;
    // Phase 14: multiple scattering approximation
    int     msApprox;
    float3  msTint;
};

struct LaunchParams {
    float4*            framebuffer;
    float*             depthbuffer;
    unsigned int       width;
    unsigned int       height;

    CameraParams       camera;
    OptixTraversableHandle traversable;

    int                spp;
    int                maxBounces;
    int                colorSpace;   // 0=sRGB, 1=ACEScg, 2=ACES2065

    float3*            normals;
    int*               materialIds;
    unsigned int       triCount;

    GPUMaterial*       materials;
    unsigned int       materialCount;

    GPULight*          lights;
    unsigned int       lightCount;

    // Per-triangle UVs (2 floats * 3 verts = 6 floats per tri)
    float2*            uvs;           // 3 per triangle (uv0, uv1, uv2)

    // Texture table
    GPUTexture*        textures;
    unsigned int       textureCount;

    int                blueNoise;        // 1 = R2 sampling, 0 = random

    // Volume data (VDB grid uploaded to GPU)
    float*             volumeDensity;     // 3D density grid (resX*resY*resZ)
    float*             volumeTemperature; // 3D temperature grid (optional, null if none)
    int                volResX, volResY, volResZ;
    float3             volBboxMin, volBboxMax;
    float              volExtinction;
    float              volScattering;
    float              volDensityMult;
    float              volGForward;
    float              volEmissionIntensity;
    float              volTempMin, volTempMax;
    float              volPowder;
    float3             volScatterColor;
    float              volStepSize;       // 0 = auto
    int                volJitter;
    int                hasVolume;         // 1 = volume active
    // Phase 12 additions
    float              volGBackward;
    float              volLobeMix;
    int                volPhaseMode;      // 0=dual-lobe HG, 1=Cornette-Shanks Mie
    float              volMieDropletD;    // microns
    int                volShadowSteps;
    float              volShadowDensity;
    float              volQuality;
    int                volAdaptiveStep;
    int                volRenderMode;     // 0=Lit,1=Grey,2=Heat,3=Cool,4=BB,5=Expl
    float              volIntensity;
    float              volFlameIntensity;
    float*             volumeFlame;       // 3D flame grid (optional)
    // Noise
    int                volNoiseEnable;
    float              volNoiseScale;
    float              volNoiseStrength;
    int                volNoiseOctaves;
    float              volNoiseRoughness;
    // Volume transform (inverse)
    int                volHasTransform;
    float3             volXfCenter;        // transformed center
    float3             volInvScale;        // 1/scale
    float              volInvRotM[9];      // inverse rotation (transpose of rotation matrix)
    float3             volOrigBboxMin;     // original bbox (before transform)
    float3             volOrigBboxMax;

    // Multi-volume support (Phase 13)
    GPUVolume              gpuVolumes[SPECTRAL_MAX_GPU_VOLUMES];
    int                    numGpuVolumes;
};

// Per-ray payload — carried through the trace
struct RayPayload {
    float3 color;
    float  depth;
    int    hit;
};

// SBT record types
struct RayGenData {};
struct MissData {};

struct HitGroupData {
    float3* normals;      // pointer to normals array (3 per triangle)
    int*    materialIds;  // pointer to material IDs (1 per triangle)
    float2* uvs;          // pointer to UVs (3 per triangle)
};

} // namespace spectral_gpu
