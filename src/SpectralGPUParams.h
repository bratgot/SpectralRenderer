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

// GPU-side material — plain data, no pointers to host objects
struct GPUMaterial {
    float3 baseColor;
    float  metallic;
    float  roughness;
    float  ior;
    float  opacity;
    float3 emissiveColor;
};

struct CameraParams {
    // Full matrices — same ray generation as CPU _MakeRay
    float projInverse[16];   // 4x4 column-major
    float viewToWorld[16];   // 4x4 column-major
};

struct LaunchParams {
    // Output
    float4*            framebuffer;
    float*             depthbuffer;
    unsigned int       width;
    unsigned int       height;

    // Camera
    CameraParams       camera;

    // Scene (OptiX traversable)
    OptixTraversableHandle traversable;

    // Render settings
    int                spp;

    // Triangle data for shading (device pointers)
    float3*            normals;       // 3 normals per triangle (n0, n1, n2)
    int*               materialIds;   // 1 per triangle
    unsigned int       triCount;

    // Material table
    GPUMaterial*       materials;
    unsigned int       materialCount;
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
};

} // namespace spectral_gpu
