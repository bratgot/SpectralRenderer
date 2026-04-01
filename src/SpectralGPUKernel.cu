// ---------------------------------------------------------------------------
// SpectralGPUKernel.cu
//
//   OptiX 8.1 device programs for the Spectral renderer.
//   Compiled to PTX at build time, loaded by SpectralGPU at runtime.
//
//   Programs:
//     __raygen__spectral      — generate camera ray, trace, write pixel
//     __closesthit__spectral  — return interpolated normal for shading
//     __miss__spectral        — sky gradient background
// ---------------------------------------------------------------------------

#include <optix_device.h>
#include <cuda_runtime.h>

#include "SpectralGPUParams.h"

using namespace spectral_gpu;

// Launch params — set by optixLaunch via the pipeline
extern "C" {
    __constant__ LaunchParams params;
}

// ---------------------------------------------------------------------------
// Payload helpers — pack/unpack through optixSetPayload / optixGetPayload
//   Slot 0: color.x (as uint)
//   Slot 1: color.y
//   Slot 2: color.z
//   Slot 3: depth
//   Slot 4: hit flag
// ---------------------------------------------------------------------------
static __forceinline__ __device__ void setPayload(const RayPayload& p)
{
    optixSetPayload_0(__float_as_uint(p.color.x));
    optixSetPayload_1(__float_as_uint(p.color.y));
    optixSetPayload_2(__float_as_uint(p.color.z));
    optixSetPayload_3(__float_as_uint(p.depth));
    optixSetPayload_4(static_cast<unsigned int>(p.hit));
}

static __forceinline__ __device__ RayPayload getPayload()
{
    RayPayload p;
    p.color.x = __uint_as_float(optixGetPayload_0());
    p.color.y = __uint_as_float(optixGetPayload_1());
    p.color.z = __uint_as_float(optixGetPayload_2());
    p.depth   = __uint_as_float(optixGetPayload_3());
    p.hit     = static_cast<int>(optixGetPayload_4());
    return p;
}

// ---------------------------------------------------------------------------
// Sky color — matches CPU path
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float3 skyColor(float3 dir)
{
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 1e-6f) {
        dir.x /= len;
        dir.y /= len;
        dir.z /= len;
    }
    float t = fmaxf(0.0f, fminf(1.0f, dir.y * 0.5f + 0.5f));
    float3 horizon = make_float3(0.72f, 0.70f, 0.68f);
    float3 zenith  = make_float3(0.35f, 0.55f, 0.82f);
    return make_float3(
        horizon.x + (zenith.x - horizon.x) * t,
        horizon.y + (zenith.y - horizon.y) * t,
        horizon.z + (zenith.z - horizon.z) * t
    );
}

// ---------------------------------------------------------------------------
// Normal-as-colour shading — matches CPU path
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float3 shadeNormal(float3 n)
{
    float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-6f) {
        n.x /= len;
        n.y /= len;
        n.z /= len;
    }
    return make_float3(n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f, n.z * 0.5f + 0.5f);
}

// ---------------------------------------------------------------------------
// __raygen__spectral
// ---------------------------------------------------------------------------
extern "C" __global__ void __raygen__spectral()
{
    const uint3 idx = optixGetLaunchIndex();
    const uint3 dim = optixGetLaunchDimensions();

    const unsigned int px = idx.x;
    const unsigned int py = idx.y;

    // Generate camera ray
    // NDC in [-1, 1]
    const float ndcX =  2.0f * (px + 0.5f) / float(dim.x) - 1.0f;
    const float ndcY = -2.0f * (py + 0.5f) / float(dim.y) + 1.0f;

    const CameraParams& cam = params.camera;
    float3 rayDir = make_float3(
        cam.U.x * ndcX * cam.tanHalfFovX + cam.V.x * ndcY * cam.tanHalfFovY + cam.W.x,
        cam.U.y * ndcX * cam.tanHalfFovX + cam.V.y * ndcY * cam.tanHalfFovY + cam.W.y,
        cam.U.z * ndcX * cam.tanHalfFovX + cam.V.z * ndcY * cam.tanHalfFovY + cam.W.z
    );

    // Initialize payload
    unsigned int p0 = 0, p1 = 0, p2 = 0, p3 = __float_as_uint(1e30f), p4 = 0;

    optixTrace(
        params.traversable,
        make_float3(cam.origin.x, cam.origin.y, cam.origin.z),
        rayDir,
        1e-4f,           // tmin
        1e30f,           // tmax
        0.0f,            // rayTime
        OptixVisibilityMask(0xFF),
        OPTIX_RAY_FLAG_NONE,
        0,               // SBT offset
        1,               // SBT stride
        0,               // missSBTIndex
        p0, p1, p2, p3, p4
    );

    float3 color = make_float3(
        __uint_as_float(p0),
        __uint_as_float(p1),
        __uint_as_float(p2)
    );
    float depth = __uint_as_float(p3);

    // Write to framebuffer
    const unsigned int pixIdx = py * dim.x + px;
    params.framebuffer[pixIdx] = make_float4(color.x, color.y, color.z, 1.0f);
    if (params.depthbuffer)
        params.depthbuffer[pixIdx] = depth;
}

// ---------------------------------------------------------------------------
// __closesthit__spectral
// ---------------------------------------------------------------------------
extern "C" __global__ void __closesthit__spectral()
{
    const unsigned int primIdx = optixGetPrimitiveIndex();
    const float2 bary = optixGetTriangleBarycentrics();
    const float  tHit = optixGetRayTmax();

    const float u = bary.x;
    const float v = bary.y;
    const float w = 1.0f - u - v;

    // Read normals from SBT hit group data
    const HitGroupData* data =
        reinterpret_cast<const HitGroupData*>(optixGetSbtDataPointer());

    float3 n0 = data->normals[primIdx * 3 + 0];
    float3 n1 = data->normals[primIdx * 3 + 1];
    float3 n2 = data->normals[primIdx * 3 + 2];

    float3 normal = make_float3(
        n0.x * w + n1.x * u + n2.x * v,
        n0.y * w + n1.y * u + n2.y * v,
        n0.z * w + n1.z * u + n2.z * v
    );

    float3 color = shadeNormal(normal);

    // Compute camera-space Z for deep output
    // Transform hit point to view space
    float3 rayOrig = optixGetWorldRayOrigin();
    float3 rayDir  = optixGetWorldRayDirection();
    float3 worldHit = make_float3(
        rayOrig.x + tHit * rayDir.x,
        rayOrig.y + tHit * rayDir.y,
        rayOrig.z + tHit * rayDir.z
    );
    // For now use ray distance — proper camera-space Z done on host
    float depth = tHit;

    optixSetPayload_0(__float_as_uint(color.x));
    optixSetPayload_1(__float_as_uint(color.y));
    optixSetPayload_2(__float_as_uint(color.z));
    optixSetPayload_3(__float_as_uint(depth));
    optixSetPayload_4(1u);
}

// ---------------------------------------------------------------------------
// __miss__spectral
// ---------------------------------------------------------------------------
extern "C" __global__ void __miss__spectral()
{
    float3 dir = optixGetWorldRayDirection();
    float3 color = skyColor(dir);

    optixSetPayload_0(__float_as_uint(color.x));
    optixSetPayload_1(__float_as_uint(color.y));
    optixSetPayload_2(__float_as_uint(color.z));
    optixSetPayload_3(__float_as_uint(1e30f));
    optixSetPayload_4(0u);
}
