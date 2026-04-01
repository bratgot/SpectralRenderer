// ---------------------------------------------------------------------------
// SpectralGPUKernel.cu
//
//   OptiX 8.1 device programs for the Spectral renderer.
//   Supports both spp=1 (normal-as-colour) and spp>1 (hero wavelength).
// ---------------------------------------------------------------------------

#include <optix_device.h>
#include <cuda_runtime.h>

#include "SpectralGPUParams.h"

using namespace spectral_gpu;

extern "C" {
    __constant__ LaunchParams params;
}

// ---------------------------------------------------------------------------
// Wang hash RNG
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float hashRNG(unsigned int seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / float(0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// CIE 1931 2-degree observer, 5nm steps 380-780nm
// ---------------------------------------------------------------------------
__device__ static const float kCIE_X[81] = {
    0.0014f, 0.0022f, 0.0042f, 0.0076f, 0.0143f,
    0.0232f, 0.0435f, 0.0776f, 0.1344f, 0.2148f,
    0.2839f, 0.3285f, 0.3483f, 0.3481f, 0.3362f,
    0.3187f, 0.2908f, 0.2511f, 0.1954f, 0.1421f,
    0.0956f, 0.0580f, 0.0320f, 0.0147f, 0.0049f,
    0.0024f, 0.0093f, 0.0291f, 0.0633f, 0.1096f,
    0.1655f, 0.2257f, 0.2904f, 0.3597f, 0.4334f,
    0.5121f, 0.5945f, 0.6784f, 0.7621f, 0.8425f,
    0.9163f, 0.9786f, 1.0263f, 1.0567f, 1.0622f,
    1.0456f, 1.0026f, 0.9384f, 0.8544f, 0.7514f,
    0.6424f, 0.5419f, 0.4479f, 0.3608f, 0.2835f,
    0.2187f, 0.1649f, 0.1212f, 0.0874f, 0.0636f,
    0.0468f, 0.0329f, 0.0227f, 0.0158f, 0.0114f,
    0.0081f, 0.0058f, 0.0041f, 0.0029f, 0.0020f,
    0.0014f, 0.0010f, 0.0007f, 0.0005f, 0.0003f,
    0.0002f, 0.0002f, 0.0001f, 0.0001f, 0.0001f,
    0.0000f
};

__device__ static const float kCIE_Y[81] = {
    0.0000f, 0.0001f, 0.0001f, 0.0002f, 0.0004f,
    0.0006f, 0.0012f, 0.0022f, 0.0040f, 0.0073f,
    0.0116f, 0.0168f, 0.0230f, 0.0298f, 0.0380f,
    0.0480f, 0.0600f, 0.0739f, 0.0910f, 0.1126f,
    0.1390f, 0.1693f, 0.2080f, 0.2586f, 0.3230f,
    0.4073f, 0.5030f, 0.6082f, 0.7100f, 0.7932f,
    0.8620f, 0.9149f, 0.9540f, 0.9803f, 0.9950f,
    1.0000f, 0.9950f, 0.9786f, 0.9520f, 0.9154f,
    0.8700f, 0.8163f, 0.7570f, 0.6949f, 0.6310f,
    0.5668f, 0.5030f, 0.4412f, 0.3810f, 0.3210f,
    0.2650f, 0.2170f, 0.1750f, 0.1382f, 0.1070f,
    0.0816f, 0.0610f, 0.0446f, 0.0320f, 0.0232f,
    0.0170f, 0.0119f, 0.0082f, 0.0057f, 0.0041f,
    0.0029f, 0.0021f, 0.0015f, 0.0010f, 0.0007f,
    0.0005f, 0.0004f, 0.0003f, 0.0002f, 0.0001f,
    0.0001f, 0.0001f, 0.0000f, 0.0000f, 0.0000f,
    0.0000f
};

__device__ static const float kCIE_Z[81] = {
    0.0065f, 0.0105f, 0.0201f, 0.0362f, 0.0679f,
    0.1102f, 0.2074f, 0.3713f, 0.6456f, 1.0391f,
    1.3856f, 1.6230f, 1.7471f, 1.7826f, 1.7721f,
    1.7441f, 1.6692f, 1.5281f, 1.2876f, 1.0419f,
    0.8130f, 0.6162f, 0.4652f, 0.3533f, 0.2720f,
    0.2123f, 0.1582f, 0.1117f, 0.0782f, 0.0573f,
    0.0422f, 0.0298f, 0.0203f, 0.0134f, 0.0088f,
    0.0058f, 0.0039f, 0.0027f, 0.0021f, 0.0018f,
    0.0017f, 0.0014f, 0.0011f, 0.0010f, 0.0008f,
    0.0006f, 0.0003f, 0.0002f, 0.0002f, 0.0001f,
    0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
    0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
    0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
    0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
    0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
    0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
    0.0000f
};

static __forceinline__ __device__ void cieXYZ(float lambda, float& cx, float& cy, float& cz)
{
    if (lambda < 380.f || lambda > 780.f) { cx = cy = cz = 0.f; return; }
    float t = (lambda - 380.f) / 5.f;
    int i = min(int(t), 79);
    float frac = t - float(i);
    cx = kCIE_X[i] * (1.f - frac) + kCIE_X[i + 1] * frac;
    cy = kCIE_Y[i] * (1.f - frac) + kCIE_Y[i + 1] * frac;
    cz = kCIE_Z[i] * (1.f - frac) + kCIE_Z[i + 1] * frac;
}

static constexpr float kLambdaRange = 400.f;
static constexpr float kCIE_Y_Integral = 106.856895f;

// ---------------------------------------------------------------------------
// Spectral sky
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float skySpectral(float3 dir, float lambda)
{
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }
    float t = fmaxf(0.f, fminf(1.f, dir.y * 0.5f + 0.5f));
    float scatter = 1.f;
    if (lambda > 400.f) {
        float ratio = 460.f / lambda;
        scatter = ratio * ratio * ratio * ratio;
    }
    return 0.7f + (0.4f * scatter - 0.7f) * t;
}

// Normal-as-colour sky
static __forceinline__ __device__ float3 skyColor(float3 dir)
{
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }
    float t = fmaxf(0.f, fminf(1.f, dir.y * 0.5f + 0.5f));
    return make_float3(
        0.72f + (0.35f - 0.72f) * t,
        0.70f + (0.55f - 0.70f) * t,
        0.68f + (0.82f - 0.68f) * t);
}

static __forceinline__ __device__ float3 shadeNormal(float3 n)
{
    float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-6f) { n.x /= len; n.y /= len; n.z /= len; }
    return make_float3(n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f, n.z * 0.5f + 0.5f);
}

// White diffuse spectral shading
static __forceinline__ __device__ float shadeSpectral(float3 normal, float lambda)
{
    float len = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len > 1e-6f) { normal.x /= len; normal.y /= len; normal.z /= len; }
    float nDotUp   = fmaxf(0.f, normal.y);
    float nDotDown = fmaxf(0.f, -normal.y);
    float nDotSide = 1.f - nDotUp - nDotDown;
    float skyAbove = skySpectral(make_float3(0.f, 1.f, 0.f), lambda);
    float skyBelow = skySpectral(make_float3(0.f, -1.f, 0.f), lambda) * 0.3f;
    float skySide  = skySpectral(make_float3(1.f, 0.f, 0.f), lambda);
    return 0.8f * (nDotUp * skyAbove + nDotDown * skyBelow + nDotSide * skySide);
}

// ---------------------------------------------------------------------------
// Get interpolated normal from hit group data
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float3 getHitNormal()
{
    const unsigned int primIdx = optixGetPrimitiveIndex();
    const float2 bary = optixGetTriangleBarycentrics();
    const float u = bary.x, v = bary.y, w = 1.f - u - v;
    const HitGroupData* data =
        reinterpret_cast<const HitGroupData*>(optixGetSbtDataPointer());
    float3 n0 = data->normals[primIdx * 3 + 0];
    float3 n1 = data->normals[primIdx * 3 + 1];
    float3 n2 = data->normals[primIdx * 3 + 2];
    return make_float3(
        n0.x * w + n1.x * u + n2.x * v,
        n0.y * w + n1.y * u + n2.y * v,
        n0.z * w + n1.z * u + n2.z * v);
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
    const unsigned int pixIdx = py * dim.x + px;
    const int spp = params.spp;
    const CameraParams& cam = params.camera;

    if (spp <= 1) {
        // ---- Normal-as-colour ----
        float ndcX =  2.f * (px + 0.5f) / float(dim.x) - 1.f;
        float ndcY = -2.f * (py + 0.5f) / float(dim.y) + 1.f;
        float3 rayDir = make_float3(
            cam.U.x * ndcX * cam.tanHalfFovX + cam.V.x * ndcY * cam.tanHalfFovY + cam.W.x,
            cam.U.y * ndcX * cam.tanHalfFovX + cam.V.y * ndcY * cam.tanHalfFovY + cam.W.y,
            cam.U.z * ndcX * cam.tanHalfFovX + cam.V.z * ndcY * cam.tanHalfFovY + cam.W.z);

        unsigned int p0=0, p1=0, p2=0, p3=__float_as_uint(1e30f), p4=0;
        optixTrace(params.traversable, cam.origin, rayDir,
                   1e-4f, 1e30f, 0.f, OptixVisibilityMask(0xFF),
                   OPTIX_RAY_FLAG_NONE, 0, 1, 0, p0, p1, p2, p3, p4);

        params.framebuffer[pixIdx] = make_float4(
            __uint_as_float(p0), __uint_as_float(p1), __uint_as_float(p2), 1.f);
        if (params.depthbuffer) params.depthbuffer[pixIdx] = __uint_as_float(p3);

    } else {
        // ---- Hero wavelength spectral ----
        float X = 0.f, Y = 0.f, Z = 0.f;
        float minDepth = 1e30f;

        for (int s = 0; s < spp; ++s) {
            unsigned int seed = pixIdx * 1031u + s * 6571u;
            float jx = hashRNG(seed);
            float jy = hashRNG(seed + 1u);
            float wu = (float(s) + hashRNG(seed + 2u)) / float(spp);
            float lambda = 380.f + wu * 400.f;

            float ndcX =  2.f * (px + jx) / float(dim.x) - 1.f;
            float ndcY = -2.f * (py + jy) / float(dim.y) + 1.f;
            float3 rayDir = make_float3(
                cam.U.x * ndcX * cam.tanHalfFovX + cam.V.x * ndcY * cam.tanHalfFovY + cam.W.x,
                cam.U.y * ndcX * cam.tanHalfFovX + cam.V.y * ndcY * cam.tanHalfFovY + cam.W.y,
                cam.U.z * ndcX * cam.tanHalfFovX + cam.V.z * ndcY * cam.tanHalfFovY + cam.W.z);

            unsigned int p0=0, p1=0, p2=0, p3=__float_as_uint(1e30f), p4=0;
            optixTrace(params.traversable, cam.origin, rayDir,
                       1e-4f, 1e30f, 0.f, OptixVisibilityMask(0xFF),
                       OPTIX_RAY_FLAG_NONE, 0, 1, 0, p0, p1, p2, p3, p4);

            float depth = __uint_as_float(p3);
            int hit = int(p4);

            float radiance;
            if (hit) {
                float3 normal = make_float3(
                    __uint_as_float(p0), __uint_as_float(p1), __uint_as_float(p2));
                radiance = shadeSpectral(normal, lambda);
                if (depth < minDepth) minDepth = depth;
            } else {
                radiance = skySpectral(rayDir, lambda);
            }

            float cx, cy, cz;
            cieXYZ(lambda, cx, cy, cz);
            float scale = kLambdaRange / kCIE_Y_Integral;
            X += radiance * cx * scale;
            Y += radiance * cy * scale;
            Z += radiance * cz * scale;
        }

        float inv = 1.f / float(spp);
        X *= inv; Y *= inv; Z *= inv;
        float r =  3.2406f * X - 1.5372f * Y - 0.4986f * Z;
        float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
        float b =  0.0557f * X - 0.2040f * Y + 1.0570f * Z;
        params.framebuffer[pixIdx] = make_float4(fmaxf(0.f,r), fmaxf(0.f,g), fmaxf(0.f,b), 1.f);
        if (params.depthbuffer) params.depthbuffer[pixIdx] = minDepth;
    }
}

// ---------------------------------------------------------------------------
// __closesthit__spectral
// ---------------------------------------------------------------------------
extern "C" __global__ void __closesthit__spectral()
{
    float3 normal = getHitNormal();
    float tHit = optixGetRayTmax();
    if (params.spp <= 1) {
        float3 color = shadeNormal(normal);
        optixSetPayload_0(__float_as_uint(color.x));
        optixSetPayload_1(__float_as_uint(color.y));
        optixSetPayload_2(__float_as_uint(color.z));
    } else {
        optixSetPayload_0(__float_as_uint(normal.x));
        optixSetPayload_1(__float_as_uint(normal.y));
        optixSetPayload_2(__float_as_uint(normal.z));
    }
    optixSetPayload_3(__float_as_uint(tHit));
    optixSetPayload_4(1u);
}

// ---------------------------------------------------------------------------
// __miss__spectral
// ---------------------------------------------------------------------------
extern "C" __global__ void __miss__spectral()
{
    if (params.spp <= 1) {
        float3 dir = optixGetWorldRayDirection();
        float3 color = skyColor(dir);
        optixSetPayload_0(__float_as_uint(color.x));
        optixSetPayload_1(__float_as_uint(color.y));
        optixSetPayload_2(__float_as_uint(color.z));
    } else {
        optixSetPayload_0(0u);
        optixSetPayload_1(0u);
        optixSetPayload_2(0u);
    }
    optixSetPayload_3(__float_as_uint(1e30f));
    optixSetPayload_4(0u);
}
