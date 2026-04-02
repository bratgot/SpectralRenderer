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

struct LaunchParams {
    float4*            framebuffer;
    float*             depthbuffer;
    unsigned int       width;
    unsigned int       height;

    CameraParams       camera;
    OptixTraversableHandle traversable;

    int                spp;
    int                maxBounces;

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
