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
    // Shadow catcher
    int    isShadowCatcher;  // 1 = shadow catcher material
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
    int    useD65;        // 1 = D65 daylight spectrum instead of RGB Gaussians
};

struct CameraParams {
    float projInverse[16];
    float viewToWorld[16];
    float viewToWorldClose[16]; // camera at shutter close (motion blur)
    int   cameraMblur;          // 0 = off, 1 = on
    float fStop;            // 0 = pinhole
    float focusDistance;
    float focalLength;      // mm
    float right[3];         // camera right vector (world)
    float up[3];            // camera up vector (world)
    float forward[3];       // camera forward vector (world)
};

#define SPECTRAL_MAX_GPU_VOLUMES 8

struct GPUVolume {
    cudaTextureObject_t densityTex;      // 3D texture (hardware trilinear)
    cudaTextureObject_t temperatureTex;  // 3D texture (0 if none)
    cudaTextureObject_t flameTex;        // 3D texture (0 if none)
    // NanoVDB device grids (alternative sparse path)
    void*   nanoGridDensity;    // device ptr to NanoGrid<float>
    void*   nanoGridTemp;       // device ptr to NanoGrid<float>
    void*   nanoGridFlame;      // device ptr to NanoGrid<float>
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
    // Phase 17: fire & explosions
    float   flameOpacity;       // flame grid reduces density (0=none, 1=full burnaway)
    float   flameTempMin;       // flame grid min temp (K) — default 1200
    float   flameTempMax;       // flame grid max temp (K) — default 3500
    float   coreGlow;           // dense core emission multiplier
    float   coreTemp;           // dense core temperature (K) — default 4000
    int     cherenkov;          // enable Cherenkov blue glow
    float   cherenkovStrength;  // Cherenkov intensity
    float   cherenkovThreshold; // density threshold for Cherenkov activation
    // Grid mixer
    float   densityMix;
    float   tempMix;
    float   flameMix;
};

struct LaunchParams {
    float4*            framebuffer;
    float*             depthbuffer;
    unsigned int       width;
    unsigned int       height;
    unsigned int       yOffset;       // strip rendering: first Y row in this launch

    CameraParams       camera;
    OptixTraversableHandle traversable;

    int                spp;
    int                volumeSpp;     // 0 = use spp, else separate vol samples
    int                maxBounces;
    int                colorSpace;   // 0=sRGB, 1=ACEScg, 2=ACES2065
    int                previewMode;  // 1=scrub preview (no shadows, 2× step)

    float3*            normals;
    int*               materialIds;
    unsigned int       triCount;
    int                hasRealGeometry;  // 1 if scene has actual triangles (not dummy GAS)

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
    int                scanlineCompat;   // 1 = direct RGB shading (no spectral XYZ)
    int                projectionMode;   // 0=perspective, 1=UV, 2=spherical
    int                edgeSamples;      // edge AA supersamples (0=disabled)

    // Wireframe overlay
    int                wireframeEnable;
    float              wireThickness;
    float              wireOpacity;
    float3             wireColor;
    int                wireDashed;
    float              wireDashLength;
    float              wireGapLength;
    int                wireNth;
    int                wireStyle;  // 0=solid,1=guide,2=architectural,3=hidden-line

    // Shadow catcher: bitmask of material IDs (supports up to 32 materials)
    unsigned int       shadowCatcherMask;

    // Shadow catcher AOV output buffer (4 floats per pixel: R=G=B=A=shadow).
    // Null when the Iop isn't asking for the pass. Written alongside the
    // alphaAccum contribution in the shadow catcher kernel branches.
    float4*            shadowCatcherAOV;

    // UV projection lookup (one entry per pixel, built CPU-side)
    int*               uvTriIndex;      // triangle index per pixel (-1 = empty)
    float*             uvBaryU;         // barycentric U per pixel
    float*             uvBaryV;         // barycentric V per pixel

    // Volume data (VDB grid uploaded as 3D CUDA texture)
    cudaTextureObject_t volumeDensity;     // 3D density texture
    cudaTextureObject_t volumeTemperature; // 3D temperature texture (0 if none)
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
    cudaTextureObject_t volumeFlame;       // 3D flame texture (0 if none)
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

    // HDRI virtual lights + SH for GPU volume lighting (Phase 15)
    struct { float3 dir; float3 color; float radius; } gpuVirtualLights[8];
    int                    numVirtualLights;
    float3                 envSH[4];       // SH L0+L1 (DC + 3 directional)
    int                    hasEnvSH;
    float                  envIntensityGPU; // dome envIntensity for volumes
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
