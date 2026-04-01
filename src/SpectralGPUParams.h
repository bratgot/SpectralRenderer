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

struct CameraParams {
    float3 origin;
    float3 U, V, W;       // camera frame vectors (right, up, -forward)
    float  tanHalfFovX;
    float  tanHalfFovY;
};

struct LaunchParams {
    // Output
    float4*            framebuffer;   // RGBA output, width * height
    float*             depthbuffer;   // per-pixel depth, width * height
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
    unsigned int       triCount;
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
    float3* normals;     // pointer to normals array (3 per triangle)
};

} // namespace spectral_gpu
