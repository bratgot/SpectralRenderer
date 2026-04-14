// ---------------------------------------------------------------------------
// SpectralGPUKernel.cu — Full feature parity with CPU path
//   Explicit lights, shadow rays, bounce rays, Disney BSDF
// ---------------------------------------------------------------------------

#include <optix_device.h>
#include <cuda_runtime.h>
#include "SpectralGPUParams.h"

// NanoVDB sparse volume sampling (Phase 22) — disabled pending OpenVDB 12 compat
// #define SPECTRAL_USE_NANOVDB
#ifdef SPECTRAL_USE_NANOVDB
#include <nanovdb/NanoVDB.h>
#include <nanovdb/math/SampleFromVoxels.h>
#endif

using namespace spectral_gpu;

extern "C" { __constant__ LaunchParams params; }

// Forward declarations for volume helpers (defined later, used in surface shadow code)
static __forceinline__ __device__ void worldToVolUV(
    const spectral_gpu::GPUVolume& vol, float px, float py, float pz,
    float& u, float& v, float& w);
static __forceinline__ __device__ float sampleDensity(
    const spectral_gpu::GPUVolume& vol, float u, float v, float w);

// ---------------------------------------------------------------------------
// RNG
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
// Matrix math — matches CPU _MakeRay
// ---------------------------------------------------------------------------
static __forceinline__ __device__ void mat4mulv4(
    const float* m, float x, float y, float z, float w,
    float& ox, float& oy, float& oz, float& ow)
{
    ox = m[0]*x + m[1]*y + m[2]*z  + m[3]*w;
    oy = m[4]*x + m[5]*y + m[6]*z  + m[7]*w;
    oz = m[8]*x + m[9]*y + m[10]*z + m[11]*w;
    ow = m[12]*x + m[13]*y + m[14]*z + m[15]*w;
}

static __forceinline__ __device__ float3 transformPoint(const float* m, float3 p)
{
    float ox, oy, oz, ow;
    mat4mulv4(m, p.x, p.y, p.z, 1.f, ox, oy, oz, ow);
    if (fabsf(ow) > 1e-8f) { ox /= ow; oy /= ow; oz /= ow; }
    return make_float3(ox, oy, oz);
}

static __forceinline__ __device__ void makeRay(
    float px, float py, float W, float H, float3& origin, float3& dir,
    unsigned int seed = 0)
{
    float ndcX = 2.f * px / W - 1.f;
    float ndcY = -2.f * py / H + 1.f;
    float nx0,ny0,nz0,nw0; mat4mulv4(params.camera.projInverse, ndcX,ndcY,-1.f,1.f, nx0,ny0,nz0,nw0);
    float nx1,ny1,nz1,nw1; mat4mulv4(params.camera.projInverse, ndcX,ndcY, 1.f,1.f, nx1,ny1,nz1,nw1);
    float3 nearPos = make_float3(nx0/nw0, ny0/nw0, nz0/nw0);
    float3 farPos  = make_float3(nx1/nw1, ny1/nw1, nz1/nw1);
    float3 worldNear = transformPoint(params.camera.viewToWorld, nearPos);
    float3 worldFar  = transformPoint(params.camera.viewToWorld, farPos);

    // Camera motion blur: lerp between open and close camera positions
    if (params.camera.cameraMblur) {
        float t = hashRNG(seed + 99u);
        float3 nearClose = transformPoint(params.camera.viewToWorldClose, nearPos);
        float3 farClose  = transformPoint(params.camera.viewToWorldClose, farPos);
        worldNear.x += (nearClose.x - worldNear.x) * t;
        worldNear.y += (nearClose.y - worldNear.y) * t;
        worldNear.z += (nearClose.z - worldNear.z) * t;
        worldFar.x  += (farClose.x  - worldFar.x)  * t;
        worldFar.y  += (farClose.y  - worldFar.y)  * t;
        worldFar.z  += (farClose.z  - worldFar.z)  * t;
    }

    origin = worldNear;
    dir = make_float3(worldFar.x-worldNear.x, worldFar.y-worldNear.y, worldFar.z-worldNear.z);

    // DOF: thin lens
    if (params.camera.fStop > 0.f && params.camera.focusDistance > 0.f) {
        float lensRadius = (params.camera.focalLength * 0.1f) / (2.f * params.camera.fStop);
        float3 fwd = make_float3(params.camera.forward[0], params.camera.forward[1], params.camera.forward[2]);
        float denom = dir.x*fwd.x + dir.y*fwd.y + dir.z*fwd.z;
        if (fabsf(denom) > 1e-8f) {
            float dirLen = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
            float tFocus = params.camera.focusDistance / (denom / dirLen);
            float3 dirN = make_float3(dir.x/dirLen, dir.y/dirLen, dir.z/dirLen);
            float3 focalPt = make_float3(origin.x + dirN.x*tFocus, origin.y + dirN.y*tFocus, origin.z + dirN.z*tFocus);

            float u1 = hashRNG(seed);
            float u2 = hashRNG(seed + 1u);
            float r = lensRadius * sqrtf(u1);
            float theta = 6.28318f * u2;
            float dx = r * cosf(theta), dy = r * sinf(theta);

            float3 rt = make_float3(params.camera.right[0], params.camera.right[1], params.camera.right[2]);
            float3 up = make_float3(params.camera.up[0], params.camera.up[1], params.camera.up[2]);
            origin.x += rt.x*dx + up.x*dy;
            origin.y += rt.y*dx + up.y*dy;
            origin.z += rt.z*dx + up.z*dy;
            dir = make_float3(focalPt.x-origin.x, focalPt.y-origin.y, focalPt.z-origin.z);
        }
    }
}

// ---------------------------------------------------------------------------
// Vector helpers
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float dot3(float3 a, float3 b)
{ return fmaxf(0.f, a.x*b.x + a.y*b.y + a.z*b.z); }

static __forceinline__ __device__ float dot3raw(float3 a, float3 b)
{ return a.x*b.x + a.y*b.y + a.z*b.z; }

static __forceinline__ __device__ float3 normalize3(float3 v)
{
    float len = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (len > 1e-8f) { v.x/=len; v.y/=len; v.z/=len; }
    return v;
}

static __forceinline__ __device__ float3 neg3(float3 v)
{ return make_float3(-v.x, -v.y, -v.z); }

static __forceinline__ __device__ float len3(float3 v)
{ return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z); }

// ---------------------------------------------------------------------------
// CIE tables
// ---------------------------------------------------------------------------
__device__ static const float kCIE_X[81] = {
    0.0014f,0.0022f,0.0042f,0.0076f,0.0143f,0.0232f,0.0435f,0.0776f,0.1344f,0.2148f,
    0.2839f,0.3285f,0.3483f,0.3481f,0.3362f,0.3187f,0.2908f,0.2511f,0.1954f,0.1421f,
    0.0956f,0.0580f,0.0320f,0.0147f,0.0049f,0.0024f,0.0093f,0.0291f,0.0633f,0.1096f,
    0.1655f,0.2257f,0.2904f,0.3597f,0.4334f,0.5121f,0.5945f,0.6784f,0.7621f,0.8425f,
    0.9163f,0.9786f,1.0263f,1.0567f,1.0622f,1.0456f,1.0026f,0.9384f,0.8544f,0.7514f,
    0.6424f,0.5419f,0.4479f,0.3608f,0.2835f,0.2187f,0.1649f,0.1212f,0.0874f,0.0636f,
    0.0468f,0.0329f,0.0227f,0.0158f,0.0114f,0.0081f,0.0058f,0.0041f,0.0029f,0.0020f,
    0.0014f,0.0010f,0.0007f,0.0005f,0.0003f,0.0002f,0.0002f,0.0001f,0.0001f,0.0001f,
    0.0000f};
__device__ static const float kCIE_Y[81] = {
    0.0000f,0.0001f,0.0001f,0.0002f,0.0004f,0.0006f,0.0012f,0.0022f,0.0040f,0.0073f,
    0.0116f,0.0168f,0.0230f,0.0298f,0.0380f,0.0480f,0.0600f,0.0739f,0.0910f,0.1126f,
    0.1390f,0.1693f,0.2080f,0.2586f,0.3230f,0.4073f,0.5030f,0.6082f,0.7100f,0.7932f,
    0.8620f,0.9149f,0.9540f,0.9803f,0.9950f,1.0000f,0.9950f,0.9786f,0.9520f,0.9154f,
    0.8700f,0.8163f,0.7570f,0.6949f,0.6310f,0.5668f,0.5030f,0.4412f,0.3810f,0.3210f,
    0.2650f,0.2170f,0.1750f,0.1382f,0.1070f,0.0816f,0.0610f,0.0446f,0.0320f,0.0232f,
    0.0170f,0.0119f,0.0082f,0.0057f,0.0041f,0.0029f,0.0021f,0.0015f,0.0010f,0.0007f,
    0.0005f,0.0004f,0.0003f,0.0002f,0.0001f,0.0001f,0.0001f,0.0000f,0.0000f,0.0000f,
    0.0000f};
__device__ static const float kCIE_Z[81] = {
    0.0065f,0.0105f,0.0201f,0.0362f,0.0679f,0.1102f,0.2074f,0.3713f,0.6456f,1.0391f,
    1.3856f,1.6230f,1.7471f,1.7826f,1.7721f,1.7441f,1.6692f,1.5281f,1.2876f,1.0419f,
    0.8130f,0.6162f,0.4652f,0.3533f,0.2720f,0.2123f,0.1582f,0.1117f,0.0782f,0.0573f,
    0.0422f,0.0298f,0.0203f,0.0134f,0.0088f,0.0058f,0.0039f,0.0027f,0.0021f,0.0018f,
    0.0017f,0.0014f,0.0011f,0.0010f,0.0008f,0.0006f,0.0003f,0.0002f,0.0002f,0.0001f,
    0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,
    0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,
    0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,0.0000f,
    0.0000f};

static __forceinline__ __device__ void cieXYZ(float lambda, float& cx, float& cy, float& cz)
{
    if (lambda < 380.f || lambda > 780.f) { cx=cy=cz=0.f; return; }
    float t = (lambda-380.f)/5.f; int i = min(int(t),79); float f = t-float(i);
    cx = kCIE_X[i]*(1.f-f)+kCIE_X[i+1]*f;
    cy = kCIE_Y[i]*(1.f-f)+kCIE_Y[i+1]*f;
    cz = kCIE_Z[i]*(1.f-f)+kCIE_Z[i+1]*f;
}

// ---------------------------------------------------------------------------
// Spectral helpers
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float spectralGauss(float l, float c, float s)
{ float t=(l-c)/s; return expf(-0.5f*t*t); }

static __forceinline__ __device__ float spectralReflectance(const GPUMaterial& m, float l)
{
    float r = m.baseColor.x*spectralGauss(l,630.f,30.f)
            + m.baseColor.y*spectralGauss(l,532.f,30.f)
            + m.baseColor.z*spectralGauss(l,460.f,25.f);
    return fmaxf(0.f, fminf(1.f, r));
}

static __forceinline__ __device__ float3 shadeNormal(float3 n)
{
    n = normalize3(n);
    return make_float3(n.x*0.5f+0.5f, n.y*0.5f+0.5f, n.z*0.5f+0.5f);
}

// ---------------------------------------------------------------------------
// Light emission
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float blackbodyNorm(float lambda_nm, float temp)
{
    if (temp < 100.f) return 0.f;
    // hc/k in nm·K = 14387768.775
    float x = 14387768.f / (lambda_nm * temp);
    if (x > 80.f) return 0.f;  // underflow guard
    // Wien peak: peakL = 2898000/T nm,  xp = hc/(peakL*k*T) ≈ 4.96511
    float peakL = 2898000.f / temp;
    float ratio = peakL / lambda_nm;
    float r5 = ratio * ratio * ratio * ratio * ratio;
    // exp(4.96511)-1 = 142.327
    return r5 * 142.327f / (expf(x) - 1.f);
}

static __forceinline__ __device__ float lightEmission(const GPULight& light, float lambda)
{
    float spectrum;
    if (light.useColorTemp) {
        spectrum = blackbodyNorm(lambda, light.colorTemperature);
    } else if (light.useD65) {
        // CIE D65 daylight approximation — cool blue-white
        // Normalized: peak at ~460nm, warm rolloff at long wavelengths
        float d65;
        if (lambda < 400.f) d65 = 0.6f;
        else if (lambda < 440.f) d65 = 0.6f + (lambda - 400.f) / 40.f * 0.5f;
        else if (lambda < 470.f) d65 = 1.1f;  // blue peak
        else if (lambda < 530.f) d65 = 1.0f;
        else if (lambda < 560.f) d65 = 0.96f;
        else if (lambda < 600.f) d65 = 0.9f;
        else if (lambda < 650.f) d65 = 0.82f;
        else d65 = 0.7f;
        float lum = light.color.x * 0.2126f + light.color.y * 0.7152f + light.color.z * 0.0722f;
        spectrum = d65 * lum;
    } else {
        spectrum = light.color.x*spectralGauss(lambda,630.f,30.f)
                 + light.color.y*spectralGauss(lambda,532.f,30.f)
                 + light.color.z*spectralGauss(lambda,460.f,25.f);
    }
    return spectrum * light.intensity;
}

static __forceinline__ __device__ float spotAttenuation(const GPULight& light, float3 hitPos)
{
    if (light.type != 4) return 1.f;
    float3 toSurface = make_float3(hitPos.x-light.position.x, hitPos.y-light.position.y, hitPos.z-light.position.z);
    toSurface = normalize3(toSurface);
    float cosTheta = dot3(toSurface, light.direction);
    if (cosTheta < light.cosConeAngle) return 0.f;
    if (cosTheta > light.cosPenumbra) return 1.f;
    float t = (cosTheta - light.cosConeAngle) / (light.cosPenumbra - light.cosConeAngle + 1e-7f);
    return t * t * (3.f - 2.f * t);
}

static __forceinline__ __device__ float lightAttenuation(const GPULight& light, float3 hitPos)
{
    if (light.type == 0 || light.type == 3) return 1.f;
    float3 d = make_float3(light.position.x-hitPos.x, light.position.y-hitPos.y, light.position.z-hitPos.z);
    float dist2 = d.x*d.x + d.y*d.y + d.z*d.z;
    // Sphere lights far from scene (>100 units) are "distant with soft shadow" — no falloff
    if (light.type == 1 && dist2 > 10000.f) return 1.f;
    float atten = 1.f / fmaxf(dist2, 0.001f);
    if (light.type == 4) atten *= spotAttenuation(light, hitPos);
    return atten;
}

// ---------------------------------------------------------------------------
// Dispersion + thin-film
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float dispersedIOR(float ior_d, float abbe, float lambda)
{
    if (abbe <= 0.f) return ior_d;
    // Sellmeier single-term dispersion
    float lum = lambda * 0.001f;  // nm → μm
    float ld = 0.5876f;
    float resonance = 0.08f + (60.f - fminf(60.f, abbe)) / 60.f * 0.20f;
    float C = resonance * resonance;
    float nd2 = ior_d * ior_d;
    float B = (nd2 - 1.f) * (ld*ld - C) / (ld*ld);
    float n2 = 1.f + B * lum*lum / (lum*lum - C);
    if (n2 < 1.f) n2 = 1.f;
    return sqrtf(n2);
}

static __forceinline__ __device__ float thinFilmFresnel(float F, float ior, float thick, float lambda, float cosT)
{
    const float nf = 1.5f;
    float sinT = sqrtf(fmaxf(0.f, 1.f - cosT*cosT));
    float sinTt = sinT / nf;
    float cosTt = sqrtf(fmaxf(0.f, 1.f - sinTt*sinTt));
    float delta = 4.f * 3.14159f * nf * thick * cosTt / lambda;
    float mod = 0.5f * cosf(delta);
    return fmaxf(0.f, fminf(1.f, F * (1.f + mod)));
}

// ---------------------------------------------------------------------------
// Disney BSDF with dispersion + thin-film
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float schlickW(float c)
{ float t=1.f-c; float t2=t*t; return t2*t2*t; }

static __forceinline__ __device__ float ggxD(float a, float NdH)
{ float a2=a*a; float d=NdH*NdH*(a2-1.f)+1.f; return a2/(3.14159f*d*d+1e-7f); }

static __forceinline__ __device__ float smithG1(float a, float NdX)
{ float a2=a*a; return (2.f*NdX)/(NdX+sqrtf(a2+(1.f-a2)*NdX*NdX)+1e-7f); }

static __forceinline__ __device__ float evalBSDF(
    const GPUMaterial& mat, float3 N, float3 V, float3 L, float lambda)
{
    float NdL=dot3(N,L); float NdV=dot3(N,V);
    if (NdL<=0.f||NdV<=0.f) return 0.f;
    float3 H=normalize3(make_float3(V.x+L.x,V.y+L.y,V.z+L.z));
    float NdH=dot3(N,H); float VdH=dot3(V,H);
    if (NdH<=0.f) return 0.f;
    float rough=fmaxf(0.01f,mat.roughness); float alpha=rough*rough;
    float baseR=spectralReflectance(mat,lambda);

    // Dispersion
    float ior = mat.ior;
    if (mat.abbeNumber > 0.f) ior = dispersedIOR(mat.ior, mat.abbeNumber, lambda);

    float iorR=(ior-1.f)/(ior+1.f); float iorF0=iorR*iorR;
    float F0=iorF0+(baseR-iorF0)*mat.metallic;
    float F=F0+(1.f-F0)*schlickW(VdH);

    // Thin-film
    if (mat.thinFilmThickness > 0.f)
        F = thinFilmFresnel(F, ior, mat.thinFilmThickness, lambda, VdH);

    float D=ggxD(alpha,NdH); float G=smithG1(alpha,NdV)*smithG1(alpha,NdL);
    float spec=(D*G*F)/(4.f*NdV*NdL+1e-7f);
    float FD90=0.5f+2.f*VdH*VdH*rough;
    float diff=(1.f-mat.metallic)*mat.opacity*baseR*(1.f+(FD90-1.f)*schlickW(NdL))*(1.f+(FD90-1.f)*schlickW(NdV))/3.14159f;
    return (diff+spec)*NdL;
}



// ---------------------------------------------------------------------------
// Fresnel + Reflect + Refract for transmission
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float3 reflect3(float3 V, float3 N)
{
    float VdN = V.x*N.x + V.y*N.y + V.z*N.z;
    return normalize3(make_float3(N.x*2.f*VdN-V.x, N.y*2.f*VdN-V.y, N.z*2.f*VdN-V.z));
}

static __forceinline__ __device__ float fresnelDielectric(float cosI, float ior)
{
    cosI = fmaxf(0.f, fminf(1.f, cosI));
    float sinT2 = (1.f - cosI*cosI) / (ior*ior);
    if (sinT2 > 1.f) return 1.f;
    float cosT = sqrtf(fmaxf(0.f, 1.f - sinT2));
    float rS = (cosI - ior*cosT) / (cosI + ior*cosT);
    float rP = (ior*cosI - cosT) / (ior*cosI + cosT);
    return 0.5f * (rS*rS + rP*rP);
}

// ---------------------------------------------------------------------------
// Tangent frame + GGX importance sampling
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float3 cross3(float3 a, float3 b)
{ return make_float3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }

static __forceinline__ __device__ void makeBasis(float3 N, float3& T, float3& B)
{
    float3 up = (fabsf(N.y)<0.999f) ? make_float3(0,1,0) : make_float3(1,0,0);
    T = normalize3(cross3(up, N));
    B = cross3(N, T);
}

// Sample bounce: reflection, refraction, or GGX/cosine importance sampling
static __forceinline__ __device__ float3 sampleBounce(
    const GPUMaterial& mat, float3 N, float3 V, float lambda,
    float u1, float u2, float& throughput, bool& transmitted, bool entering)
{
    transmitted = false;

    // Transparent dielectric: Fresnel-weighted reflect vs refract
    if (mat.opacity < 0.99f && mat.metallic < 0.5f) {
        float ior = mat.ior;
        if (mat.abbeNumber > 0.f) ior = dispersedIOR(mat.ior, mat.abbeNumber, lambda);

        float eta = entering ? (1.f / ior) : ior;
        float3 geoN = entering ? N : neg3(N);
        float cosI = fabsf(geoN.x*V.x + geoN.y*V.y + geoN.z*V.z);
        float F = fresnelDielectric(cosI, entering ? ior : 1.f/ior);

        if (u1 < F) {
            throughput = 1.f;
            return reflect3(V, N);
        } else {
            float sinT2 = eta*eta*(1.f - cosI*cosI);
            if (sinT2 > 1.f) { throughput = 1.f; return reflect3(V, N); }
            float cosT = sqrtf(1.f - sinT2);
            float3 refr = normalize3(make_float3(
                -V.x*eta + geoN.x*(eta*cosI - cosT),
                -V.y*eta + geoN.y*(eta*cosI - cosT),
                -V.z*eta + geoN.z*(eta*cosI - cosT)));
            transmitted = true;
            throughput = 1.f;
            return refr;
        }
    }

    // Opaque: GGX + cosine importance sampling
    float rough = fmaxf(0.01f, mat.roughness);
    float alpha = rough * rough;
    float specProb = fmaxf(0.25f, 0.5f*(1.f-rough) + 0.5f*mat.metallic);

    float3 T, B;
    makeBasis(N, T, B);
    float3 L;
    float pdf;

    if (u1 < specProb) {
        float xi1 = u1 / specProb;
        float theta = atanf(alpha * sqrtf(xi1) / sqrtf(fmaxf(1e-8f, 1.f-xi1)));
        float phi = 6.28318f * u2;
        float sinT = sinf(theta), cosT = cosf(theta);
        float3 H = normalize3(make_float3(
            T.x*sinT*cosf(phi) + B.x*sinT*sinf(phi) + N.x*cosT,
            T.y*sinT*cosf(phi) + B.y*sinT*sinf(phi) + N.y*cosT,
            T.z*sinT*cosf(phi) + B.z*sinT*sinf(phi) + N.z*cosT));
        float VdH = dot3(V, H);
        if (VdH <= 0.f) { throughput = 0.f; return N; }
        L = normalize3(make_float3(H.x*2.f*VdH-V.x, H.y*2.f*VdH-V.y, H.z*2.f*VdH-V.z));
        float NdH = dot3(N, H); float NdL = dot3(N, L);
        if (NdL <= 0.f) { throughput = 0.f; return L; }
        float D = ggxD(alpha, NdH);
        float pdfGGX = D * NdH / (4.f * VdH + 1e-7f);
        pdf = specProb * pdfGGX + (1.f - specProb) * NdL / 3.14159f;
    } else {
        float xi1 = (u1 - specProb) / (1.f - specProb);
        float r = sqrtf(xi1); float phi = 6.28318f * u2;
        float x=r*cosf(phi), y=r*sinf(phi), z=sqrtf(fmaxf(0.f,1.f-xi1));
        L = normalize3(make_float3(T.x*x+B.x*y+N.x*z, T.y*x+B.y*y+N.y*z, T.z*x+B.z*y+N.z*z));
        float NdL = dot3(N, L);
        if (NdL <= 0.f) { throughput = 0.f; return L; }
        float pdfCos = NdL / 3.14159f;
        float3 H = normalize3(make_float3(V.x+L.x, V.y+L.y, V.z+L.z));
        float NdH = dot3(N, H); float VdH = dot3(V, H);
        float D = ggxD(alpha, NdH);
        float pdfGGX = (VdH > 1e-7f) ? D * NdH / (4.f * VdH) : 0.f;
        pdf = specProb * pdfGGX + (1.f - specProb) * pdfCos;
    }

    float NdL = dot3(N, L);
    if (NdL <= 0.f || pdf < 1e-7f) { throughput = 0.f; return L; }
    throughput = evalBSDF(mat, N, V, L, lambda) / pdf;
    return L;
}

// ---------------------------------------------------------------------------
// Hit helpers
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float3 getHitNormal()
{
    unsigned int pi = optixGetPrimitiveIndex();
    float2 bary = optixGetTriangleBarycentrics();
    float u=bary.x, v=bary.y, w=1.f-u-v;
    const HitGroupData* d = reinterpret_cast<const HitGroupData*>(optixGetSbtDataPointer());
    float3 n0=d->normals[pi*3], n1=d->normals[pi*3+1], n2=d->normals[pi*3+2];
    return make_float3(n0.x*w+n1.x*u+n2.x*v, n0.y*w+n1.y*u+n2.y*v, n0.z*w+n1.z*u+n2.z*v);
}

static __forceinline__ __device__ int getHitMaterialId()
{
    const HitGroupData* d = reinterpret_cast<const HitGroupData*>(optixGetSbtDataPointer());
    return d->materialIds[optixGetPrimitiveIndex()];
}

static __forceinline__ __device__ float2 getHitUV()
{
    unsigned int pi = optixGetPrimitiveIndex();
    float2 bary = optixGetTriangleBarycentrics();
    float u=bary.x, v=bary.y, w=1.f-u-v;
    const HitGroupData* d = reinterpret_cast<const HitGroupData*>(optixGetSbtDataPointer());
    float2 uv0=d->uvs[pi*3], uv1=d->uvs[pi*3+1], uv2=d->uvs[pi*3+2];
    return make_float2(uv0.x*w+uv1.x*u+uv2.x*v, uv0.y*w+uv1.y*u+uv2.y*v);
}

// Bilinear texture sampling on GPU
static __forceinline__ __device__ float3 sampleTextureGPU(int texId, float2 uv)
{
    if (texId < 0 || texId >= (int)params.textureCount) return make_float3(1,0,1);
    const GPUTexture& tex = params.textures[texId];
    if (!tex.pixels || tex.width <= 0 || tex.height <= 0) return make_float3(1,0,1);

    // Repeat wrap
    float su = uv.x - floorf(uv.x);
    float sv = 1.f - (uv.y - floorf(uv.y)); // flip V

    float fx = su * (tex.width - 1);
    float fy = sv * (tex.height - 1);
    int x0 = max(0, min(int(fx), tex.width-1));
    int y0 = max(0, min(int(fy), tex.height-1));
    int x1 = min(x0+1, tex.width-1);
    int y1 = min(y0+1, tex.height-1);
    float dx = fx - x0, dy = fy - y0;

    int ch = tex.channels;
    auto px = [&](int x, int y) -> float3 {
        int idx = (y * tex.width + x) * ch;
        float r = tex.pixels[idx];
        float g = ch >= 2 ? tex.pixels[idx+1] : r;
        float b = ch >= 3 ? tex.pixels[idx+2] : r;
        return make_float3(r,g,b);
    };

    float3 c00=px(x0,y0), c10=px(x1,y0), c01=px(x0,y1), c11=px(x1,y1);
    float3 top = make_float3(c00.x*(1-dx)+c10.x*dx, c00.y*(1-dx)+c10.y*dx, c00.z*(1-dx)+c10.z*dx);
    float3 bot = make_float3(c01.x*(1-dx)+c11.x*dx, c01.y*(1-dx)+c11.y*dx, c01.z*(1-dx)+c11.z*dx);
    return make_float3(top.x*(1-dy)+bot.x*dy, top.y*(1-dy)+bot.y*dy, top.z*(1-dy)+bot.z*dy);
}

// ---------------------------------------------------------------------------
// Sample a random point on a light surface for soft shadows
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float3 sampleLightDir(
    const GPULight& light, float3 hitPos, float u1, float u2, float3 surfNormal)
{
    if (light.type == 0)  // distant
        return normalize3(neg3(light.direction));

    if (light.type == 3) {  // dome — cosine-weighted hemisphere around surface normal
        float3 N = normalize3(surfNormal);
        float3 up2 = (fabsf(N.y) < 0.999f) ? make_float3(0,1,0) : make_float3(1,0,0);
        float3 T = normalize3(cross3(up2, N));
        float3 B = cross3(N, T);
        float r = sqrtf(u1);
        float phi = 6.28318f * u2;
        float x = r * cosf(phi), y = r * sinf(phi);
        float z = sqrtf(fmaxf(0.f, 1.f - u1));
        return normalize3(make_float3(T.x*x+B.x*y+N.x*z, T.y*x+B.y*y+N.y*z, T.z*x+B.z*y+N.z*z));
    }

    float3 samplePos = light.position;

    if ((light.type == 1 || light.type == 4) && light.radius > 0.f) {
        // Sphere or Spot: uniform point on sphere surface
        float theta = 6.28318f * u1;
        float phi = acosf(1.f - 2.f * u2);
        float sp = sinf(phi);
        samplePos.x += light.radius * sp * cosf(theta);
        samplePos.y += light.radius * sp * sinf(theta);
        samplePos.z += light.radius * cosf(phi);
    } else if (light.type == 2) {
        // Rect: random point on rectangle
        samplePos.x += (u1 - 0.5f) * light.width;
        samplePos.y += (u2 - 0.5f) * light.height;
    }

    float3 d = make_float3(samplePos.x-hitPos.x, samplePos.y-hitPos.y, samplePos.z-hitPos.z);
    return normalize3(d);
}

// ---------------------------------------------------------------------------
// Shade one hit — direct lights with soft shadows
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float shadeHit(
    const GPUMaterial& mat, float3 N, float3 V, float3 hitPos, float lambda,
    unsigned int& rngSeed)
{
    float radiance = 0.f;

    if (params.lightCount > 0) {
        for (unsigned int li = 0; li < params.lightCount; ++li) {
            const GPULight& light = params.lights[li];
            // Jittered light direction for soft shadows
            float3 L = sampleLightDir(light, hitPos, hashRNG(rngSeed++), hashRNG(rngSeed++), N);

            // Shadow ray — trace through glass (transparent shadows)
            bool inShadow = false;
            float shadowTransmit = 1.f;
            float3 sOrig = make_float3(hitPos.x+N.x*0.01f, hitPos.y+N.y*0.01f, hitPos.z+N.z*0.01f);

            for (int sb = 0; sb < 8; ++sb) {
                unsigned int sp0=0,sp1=0,sp2=0,sp3=__float_as_uint(1e30f),sp4=0,sp5=0,sp6=0;
                optixTrace(params.traversable, sOrig, L,
                           1e-4f, 1e30f, 0.f, OptixVisibilityMask(0xFF),
                           OPTIX_RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                           0, 1, 0, sp0,sp1,sp2,sp3,sp4,sp5,sp6);

                if (sp4 == 0u) break;  // no hit — reached light

                // Check if hit is glass
                int sMatId = int(sp4) - 1;
                if (sMatId >= 0 && sMatId < int(params.materialCount)) {
                    const GPUMaterial& sMat = params.materials[sMatId];
                    if (sMat.opacity < 0.99f && sMat.metallic < 0.5f) {
                        // Glass: transmit through
                        shadowTransmit *= (1.f - sMat.opacity) * 0.95f;
                        if (shadowTransmit < 0.01f) { inShadow = true; break; }
                        float sDist = __uint_as_float(sp3);
                        sOrig = make_float3(sOrig.x+L.x*(sDist+0.02f),
                                            sOrig.y+L.y*(sDist+0.02f),
                                            sOrig.z+L.z*(sDist+0.02f));
                        continue;  // trace past the glass
                    }
                }

                // Opaque blocker
                if (light.type == 0 || light.type == 3) {
                    inShadow = true;
                } else {
                    float3 toLight = make_float3(light.position.x-hitPos.x,
                        light.position.y-hitPos.y, light.position.z-hitPos.z);
                    if (__uint_as_float(sp3) < len3(toLight)) inShadow = true;
                }
                break;
            }

            if (!inShadow) {
                // March shadow ray through volumes (cloud/fog shadows on surfaces)
                if (params.numGpuVolumes > 0 && shadowTransmit > 0.01f) {
                    for (int vi = 0; vi < params.numGpuVolumes; ++vi) {
                        const GPUVolume& svol = params.gpuVolumes[vi];
                        if (!svol.densityTex) continue;

                        // Ray-AABB for shadow ray
                        float3 sInvDir = make_float3(
                            1.f/(fabsf(L.x)>1e-8f?L.x:1e-8f),
                            1.f/(fabsf(L.y)>1e-8f?L.y:1e-8f),
                            1.f/(fabsf(L.z)>1e-8f?L.z:1e-8f));
                        float3 st0=make_float3(
                            (svol.bboxMin.x-hitPos.x)*sInvDir.x,
                            (svol.bboxMin.y-hitPos.y)*sInvDir.y,
                            (svol.bboxMin.z-hitPos.z)*sInvDir.z);
                        float3 st1=make_float3(
                            (svol.bboxMax.x-hitPos.x)*sInvDir.x,
                            (svol.bboxMax.y-hitPos.y)*sInvDir.y,
                            (svol.bboxMax.z-hitPos.z)*sInvDir.z);
                        float stNear=fmaxf(fmaxf(fminf(st0.x,st1.x),fminf(st0.y,st1.y)),fminf(st0.z,st1.z));
                        float stFar=fminf(fminf(fmaxf(st0.x,st1.x),fmaxf(st0.y,st1.y)),fmaxf(st0.z,st1.z));
                        stNear=fmaxf(stNear,0.f);
                        if (stNear >= stFar) continue;

                        // Coarse march through volume
                        float svoxel = fmaxf(fmaxf(
                            (svol.bboxMax.x-svol.bboxMin.x)/svol.resX,
                            (svol.bboxMax.y-svol.bboxMin.y)/svol.resY),
                            (svol.bboxMax.z-svol.bboxMin.z)/svol.resZ);
                        float sDt = svoxel * 2.f;
                        int maxSS = min(32, int((stFar-stNear)/sDt)+1);
                        for (int ss = 0; ss < maxSS; ++ss) {
                            float st = stNear + ss * sDt;
                            if (st >= stFar) break;
                            float spx=hitPos.x+L.x*st, spy=hitPos.y+L.y*st, spz=hitPos.z+L.z*st;
                            float su,sv,sw;
                            worldToVolUV(svol, spx, spy, spz, su, sv, sw);
                            if (su<0||su>1||sv<0||sv>1||sw<0||sw>1) continue;
                            float d = sampleDensity(svol, su, sv, sw);
                            if (d < 1e-5f) continue;
                            shadowTransmit *= expf(-d * svol.extinction * sDt);
                            if (shadowTransmit < 0.01f) break;
                        }
                        if (shadowTransmit < 0.01f) break;
                    }
                }

                float bsdf = evalBSDF(mat, N, V, L, lambda);
                float emission = lightEmission(light, lambda) * lightAttenuation(light, hitPos);
                // Dome: scale by 0.5 to match CPU MIS (other half comes from bounce miss)
                if (light.type == 3) emission *= 0.5f;
                radiance += bsdf * emission * shadowTransmit;
            }
        }
    }

    // Phase 14: Diffraction grating
    if (mat.gratingSpacing > 0.f && mat.gratingStrength > 0.f) {
        float d = mat.gratingSpacing;  // um
        float lambdaUm = lambda * 0.001f;
        float cosI = fabsf(N.x*V.x + N.y*V.y + N.z*V.z);
        float sinI = sqrtf(fmaxf(0.f, 1.f - cosI*cosI));
        float diffracted = 0.f;
        for (int m = 1; m <= 3; ++m) {
            float sinM = sinI - float(m) * lambdaUm / d;
            if (sinM > -1.f && sinM < 1.f) {
                float cosM = sqrtf(1.f - sinM*sinM);
                diffracted += cosM / float(m * m);
            }
        }
        float gratingContrib = diffracted * mat.gratingStrength;
        for (unsigned li = 0; li < params.lightCount; ++li) {
            const GPULight& light = params.lights[li];
            float lightRad = lightEmission(light, lambda);
            float atten = lightAttenuation(light, hitPos);
            radiance += gratingContrib * lightRad * atten * 0.2f;
        }
    }

    // Phase 14: Fluorescence (Stokes shift)
    if (mat.fluorStrength > 0.f && mat.fluorAbsorb > 0.f) {
        float absCenter = mat.fluorAbsorb;
        float emCenter = mat.fluorEmit;
        float dAbs = lambda - absCenter;
        float absProb = expf(-dAbs * dAbs / (2.f * 30.f * 30.f));
        float dEm = lambda - emCenter;
        float emSpectrum = expf(-dEm * dEm / (2.f * 40.f * 40.f));
        float fluorContrib = absProb * emSpectrum * mat.fluorStrength * 0.5f;
        for (unsigned li = 0; li < params.lightCount; ++li) {
            const GPULight& light = params.lights[li];
            float lightRad = lightEmission(light, absCenter);
            float atten = lightAttenuation(light, hitPos);
            radiance += fluorContrib * lightRad * atten;
        }
    }

    return radiance;
}

// ---------------------------------------------------------------------------
// GPU Volume ray marching — Beer-Lambert + HG phase + emission
// ---------------------------------------------------------------------------

// Per-volume sampling functions — CUDA 3D texture with hardware trilinear
// Normalized coordinates [0,1], clamped at borders, hardware-interpolated.
// Each tex3D replaces 8 global memory reads + 7 lerps with a single texture fetch.

#ifdef SPECTRAL_USE_NANOVDB
// NanoVDB helper: sample sparse grid directly on GPU (no dense resample needed)
static __forceinline__ __device__ float sampleNanoGrid(
    const void* gridPtr, float u, float v, float w,
    const spectral_gpu::GPUVolume& vol)
{
    auto* grid = reinterpret_cast<const nanovdb::NanoGrid<float>*>(gridPtr);
    float3 orig_min = vol.hasTransform ? vol.origBboxMin : vol.bboxMin;
    float3 orig_max = vol.hasTransform ? vol.origBboxMax : vol.bboxMax;
    float wx = orig_min.x + u * (orig_max.x - orig_min.x);
    float wy = orig_min.y + v * (orig_max.y - orig_min.y);
    float wz = orig_min.z + w * (orig_max.z - orig_min.z);
    auto ipos = grid->worldToIndexF(nanovdb::Vec3f(wx, wy, wz));
    auto acc = grid->getAccessor();
    return nanovdb::math::SampleFromVoxels<decltype(acc), 1>(acc)(ipos);
}
#endif

static __forceinline__ __device__ float sampleDensity(const spectral_gpu::GPUVolume& vol, float u, float v, float w)
{
#ifdef SPECTRAL_USE_NANOVDB
    if (vol.nanoGridDensity)
        return sampleNanoGrid(vol.nanoGridDensity, u, v, w, vol) * vol.densityMult;
#endif
    return tex3D<float>(vol.densityTex, u, v, w) * vol.densityMult;
}

static __forceinline__ __device__ float sampleTemp(const spectral_gpu::GPUVolume& vol, float u, float v, float w)
{
#ifdef SPECTRAL_USE_NANOVDB
    if (vol.nanoGridTemp)
        return sampleNanoGrid(vol.nanoGridTemp, u, v, w, vol);
#endif
    if (!vol.temperatureTex) return 0.f;
    return tex3D<float>(vol.temperatureTex, u, v, w);
}

static __forceinline__ __device__ float sampleFlame(const spectral_gpu::GPUVolume& vol, float u, float v, float w)
{
#ifdef SPECTRAL_USE_NANOVDB
    if (vol.nanoGridFlame)
        return sampleNanoGrid(vol.nanoGridFlame, u, v, w, vol);
#endif
    if (!vol.flameTex) return 0.f;
    return tex3D<float>(vol.flameTex, u, v, w);
}

static __forceinline__ __device__ void worldToVolUV(
    const spectral_gpu::GPUVolume& vol, float px, float py, float pz,
    float& u, float& v, float& w)
{
    if (vol.hasTransform) {
        float lx=px-vol.xfCenter.x, ly=py-vol.xfCenter.y, lz=pz-vol.xfCenter.z;
        float rx=vol.invRotM[0]*lx+vol.invRotM[3]*ly+vol.invRotM[6]*lz;
        float ry=vol.invRotM[1]*lx+vol.invRotM[4]*ly+vol.invRotM[7]*lz;
        float rz=vol.invRotM[2]*lx+vol.invRotM[5]*ly+vol.invRotM[8]*lz;
        rx*=vol.invScale.x; ry*=vol.invScale.y; rz*=vol.invScale.z;
        float3 oc=make_float3(
            (vol.origBboxMin.x+vol.origBboxMax.x)*0.5f,
            (vol.origBboxMin.y+vol.origBboxMax.y)*0.5f,
            (vol.origBboxMin.z+vol.origBboxMax.z)*0.5f);
        float3 oh=make_float3(
            (vol.origBboxMax.x-vol.origBboxMin.x)*0.5f,
            (vol.origBboxMax.y-vol.origBboxMin.y)*0.5f,
            (vol.origBboxMax.z-vol.origBboxMin.z)*0.5f);
        u=(oh.x>1e-6f)?(rx+oc.x-vol.origBboxMin.x)/(oh.x*2.f):0.5f;
        v=(oh.y>1e-6f)?(ry+oc.y-vol.origBboxMin.y)/(oh.y*2.f):0.5f;
        w=(oh.z>1e-6f)?(rz+oc.z-vol.origBboxMin.z)/(oh.z*2.f):0.5f;
    } else {
        float3 bSize=make_float3(vol.bboxMax.x-vol.bboxMin.x,
                                  vol.bboxMax.y-vol.bboxMin.y,
                                  vol.bboxMax.z-vol.bboxMin.z);
        u=(bSize.x>1e-6f)?(px-vol.bboxMin.x)/bSize.x:0.5f;
        v=(bSize.y>1e-6f)?(py-vol.bboxMin.y)/bSize.y:0.5f;
        w=(bSize.z>1e-6f)?(pz-vol.bboxMin.z)/bSize.z:0.5f;
    }
}

// Legacy wrappers — keep old call sites working during transition
static __forceinline__ __device__ float sampleVolumeDensity(float u, float v, float w) {
    return (params.numGpuVolumes > 0) ? sampleDensity(params.gpuVolumes[0], u, v, w) : 0.f;
}
static __forceinline__ __device__ float sampleVolumeTemp(float u, float v, float w) {
    return (params.numGpuVolumes > 0) ? sampleTemp(params.gpuVolumes[0], u, v, w) : 0.f;
}
static __forceinline__ __device__ float sampleVolumeFlame(float u, float v, float w) {
    return (params.numGpuVolumes > 0) ? sampleFlame(params.gpuVolumes[0], u, v, w) : 0.f;
}
static __forceinline__ __device__ void worldToVolumeUV(float px, float py, float pz,
    float& u, float& v, float& w) {
    if (params.numGpuVolumes > 0) worldToVolUV(params.gpuVolumes[0], px, py, pz, u, v, w);
    else { u = v = w = 0.5f; }
}

// GPU fBm noise - deterministic hash-based
static __forceinline__ __device__ float gpuNoiseHash3(float x, float y, float z)
{
    int ix = __float2int_rd(x), iy = __float2int_rd(y), iz = __float2int_rd(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx*fx*(3.f-2.f*fx); fy = fy*fy*(3.f-2.f*fy); fz = fz*fz*(3.f-2.f*fz);
    auto h = [](int a, int b, int c) -> float {
        unsigned u = (unsigned)a*1664525u ^ (unsigned)b*1013904223u ^ (unsigned)c*2246822519u;
        u ^= u>>16; u *= 0x45d9f3bu; u ^= u>>16;
        return (int)(u & 0xFFFFu) / 32768.f - 1.f;
    };
    float v000=h(ix,iy,iz),     v100=h(ix+1,iy,iz);
    float v010=h(ix,iy+1,iz),   v110=h(ix+1,iy+1,iz);
    float v001=h(ix,iy,iz+1),   v101=h(ix+1,iy,iz+1);
    float v011=h(ix,iy+1,iz+1), v111=h(ix+1,iy+1,iz+1);
    return v000 + fx*(v100-v000) + fy*(v010-v000+fx*(v110-v010-v100+v000))
         + fz*(v001-v000+fx*(v101-v001-v100+v000)+fy*(v011-v001-v010+v000+fx*(v111-v011-v101+v001-v110+v010+v100-v000)));
}

static __forceinline__ __device__ float gpuNoiseFBm(float x, float y, float z, int oct, float rough)
{
    float val=0, amp=1, total=0, freq=1;
    for (int i=0; i<oct; ++i) { val+=amp*gpuNoiseHash3(x*freq,y*freq,z*freq); total+=amp; amp*=rough; freq*=2.f; }
    return total>0 ? val/total : 0;
}

// March a single volume - takes GPUVolume reference (Phase 13)
static __forceinline__ __device__ void marchSingleVolume(
    const spectral_gpu::GPUVolume& vol,
    float3 ro, float3 rdRaw, float surfaceT, float lambda, unsigned int seed,
    float3& outRGB, float& outTransmittance)
{
    outRGB = make_float3(0.f, 0.f, 0.f);
    outTransmittance = 1.f;

    float rdLen = sqrtf(rdRaw.x*rdRaw.x + rdRaw.y*rdRaw.y + rdRaw.z*rdRaw.z);
    if (rdLen < 1e-8f) return;
    float3 rd = make_float3(rdRaw.x/rdLen, rdRaw.y/rdLen, rdRaw.z/rdLen);
    surfaceT *= rdLen;

    // Ray-AABB
    float3 invDir = make_float3(
        1.f/(fabsf(rd.x)>1e-8f?rd.x:1e-8f),
        1.f/(fabsf(rd.y)>1e-8f?rd.y:1e-8f),
        1.f/(fabsf(rd.z)>1e-8f?rd.z:1e-8f));
    float3 t0=make_float3((vol.bboxMin.x-ro.x)*invDir.x,(vol.bboxMin.y-ro.y)*invDir.y,(vol.bboxMin.z-ro.z)*invDir.z);
    float3 t1=make_float3((vol.bboxMax.x-ro.x)*invDir.x,(vol.bboxMax.y-ro.y)*invDir.y,(vol.bboxMax.z-ro.z)*invDir.z);
    float tNear=fmaxf(fmaxf(fminf(t0.x,t1.x),fminf(t0.y,t1.y)),fminf(t0.z,t1.z));
    float tFar=fminf(fminf(fmaxf(t0.x,t1.x),fmaxf(t0.y,t1.y)),fmaxf(t0.z,t1.z));
    tNear=fmaxf(tNear,0.f); tFar=fminf(tFar,surfaceT);
    if (tNear>=tFar) return;

    float3 bSize=make_float3(vol.bboxMax.x-vol.bboxMin.x,vol.bboxMax.y-vol.bboxMin.y,vol.bboxMax.z-vol.bboxMin.z);
    float bboxDiag=sqrtf(bSize.x*bSize.x+bSize.y*bSize.y+bSize.z*bSize.z);
    float voxelSize=fmaxf(fmaxf(bSize.x/vol.resX,bSize.y/vol.resY),bSize.z/vol.resZ);
    float q=fmaxf(1.f,vol.quality);
    float dt=(vol.stepSize>0.01f)?vol.stepSize:voxelSize/(q*q*0.25f);
    // Preview mode: 2× step size for speed
    if (params.previewMode) dt *= 2.f;
    int maxSteps=min(params.previewMode ? 256 : 1024, int((tFar-tNear)/dt)+1);

    float jitterOff=vol.jitter?hashRNG(seed)*dt:0.f;
    float t=tNear+jitterOff;
    int rmode=vol.renderMode;

    for (int step=0; step<maxSteps && t<tFar; ++step, t+=dt) {
        float px=ro.x+rd.x*t, py=ro.y+rd.y*t, pz=ro.z+rd.z*t;
        float u,v,w;
        worldToVolUV(vol, px, py, pz, u, v, w);

        float density = sampleDensity(vol, u, v, w);

        // Procedural noise (skip in preview mode)
        if (!params.previewMode && vol.noiseEnable && density > 1e-6f) {
            float ns = vol.noiseScale / fmaxf(bboxDiag, 1e-4f);
            float n = gpuNoiseFBm(px*ns, py*ns, pz*ns,
                                   vol.noiseOctaves, vol.noiseRoughness);
            density = fmaxf(0.f, density * (1.f + vol.noiseStrength * n));
        }

        if (density < 1e-5f) { if (vol.adaptiveStep) t+=dt*3.f; continue; }

        // Phase 17: flame opacity — fire burns away density
        float flameVal = sampleFlame(vol, u, v, w) * vol.flameMix;
        if (vol.flameOpacity > 0.01f && flameVal > 0.01f) {
            density = fmaxf(0.f, density * (1.f - flameVal * vol.flameOpacity));
            if (density < 1e-5f) continue;
        }

        // Grid mixer: fade density
        if (vol.densityMix < 0.99f) {
            density *= vol.densityMix;
            if (density < 1e-5f) continue;
        }

        float sigma_t = density * vol.extinction;
        // Phase 14: chromatic extinction — wavelength-dependent absorption
        if (vol.chromaticExtinction) {
            float wt = (lambda < 500.f) ? vol.sigmaB : (lambda < 580.f) ? vol.sigmaG : vol.sigmaR;
            sigma_t *= wt;
        }
        float stepTrans = expf(-sigma_t*dt);
        float absorption = 1.f - stepTrans;
        float3 stepRGB = make_float3(0.f, 0.f, 0.f);

        if (rmode==1) {
            float vv=density*vol.intensity; stepRGB=make_float3(vv,vv,vv);
        } else if (rmode==2||rmode==3) {
            float tt=fminf(density*vol.extinction*0.5f,1.f)*vol.intensity;
            stepRGB=make_float3(fminf(tt*3.f,1.f),fmaxf(0.f,(tt-.33f)*3.f),fmaxf(0.f,(tt-.66f)*3.f));
        } else if (rmode==4) {
            float temp = sampleTemp(vol, u, v, w);
            if (temp > vol.tempMin) {
                float tN=fminf((temp-vol.tempMin)/(vol.tempMax-vol.tempMin+1e-6f),1.f);
                float T=vol.tempMin+tN*(vol.tempMax-vol.tempMin);
                float t100b=T/100.f; float cr=1,cg=1,cb=1;
                if(T<=6600.f){cr=1;cg=fmaxf(0.f,fminf(0.39f*logf(t100b)-0.63f,1.f));
                    cb=fmaxf(0.f,fminf(0.54f*logf(t100b-10.f)-1.19f,1.f));}
                else{cr=fmaxf(0.f,fminf(1.29f*powf(t100b-60.f,-0.13f),1.f));
                    cg=fmaxf(0.f,fminf(1.13f*powf(t100b-60.f,-0.07f),1.f));cb=1;}
                float em=vol.emissionIntensity*density*vol.intensity;
                stepRGB=make_float3(cr*em,cg*em,cb*em);
            }
        } else { // Lit (0) / Explosion (5)
            float powder=1.f;
            if (vol.powder>0.01f) powder=1.f-expf(-density*vol.powder*2.f);

            for (unsigned li=0; li<params.lightCount; ++li) {
                const GPULight& L = params.lights[li];
                if (L.type==3) continue;
                float lI=(L.color.x+L.color.y+L.color.z)*0.333f*L.intensity;
                if (lI<0.001f) continue;

                float3 lDir;
                if (L.type==1||L.type==4) {
                    // Sphere/spot light — jitter on sphere surface for soft shadows
                    float3 lightPos = L.position;
                    if (L.radius > 0.01f) {
                        unsigned int jSeed = seed + step * 131u + li * 37u;
                        float ju1 = hashRNG(jSeed);
                        float ju2 = hashRNG(jSeed + 0x9e3779b9u);
                        float jTheta = 6.28318f * ju1;
                        float jPhi = acosf(1.f - 2.f * ju2);
                        float jsp = sinf(jPhi);
                        lightPos.x += L.radius * jsp * cosf(jTheta);
                        lightPos.y += L.radius * jsp * sinf(jTheta);
                        lightPos.z += L.radius * cosf(jPhi);
                    }
                    lDir=make_float3(px-lightPos.x,py-lightPos.y,pz-lightPos.z);
                    float dl=sqrtf(lDir.x*lDir.x+lDir.y*lDir.y+lDir.z*lDir.z);
                    if(dl>1e-6f){lDir.x/=dl;lDir.y/=dl;lDir.z/=dl;}
                } else {
                    float dl=sqrtf(L.direction.x*L.direction.x+L.direction.y*L.direction.y+L.direction.z*L.direction.z);
                    lDir=(dl>1e-6f)?make_float3(L.direction.x/dl,L.direction.y/dl,L.direction.z/dl):make_float3(0,-1,0);
                }
                float3 lCol=make_float3(L.color.x*L.intensity,L.color.y*L.intensity,L.color.z*L.intensity);
                float cosTheta=-(rd.x*lDir.x+rd.y*lDir.y+rd.z*lDir.z);
                float phase;
                if (vol.phaseMode==1) {
                    float d=vol.mieDropletD;
                    float g=0.85f*(1.f-expf(-d*0.8f));
                    float g2=g*g;
                    float num=(1.f-g2)*(1.f+cosTheta*cosTheta);
                    float denom=(2.f+g2)*powf(fmaxf(1.f+g2-2.f*g*cosTheta,1e-4f),1.5f);
                    phase=(3.f/(8.f*3.14159f))*num/denom;
                } else {
                    float gF=vol.gForward, gB=vol.gBackward;
                    float denomF=1.f+gF*gF-2.f*gF*cosTheta, denomB=1.f+gB*gB-2.f*gB*cosTheta;
                    float phaseF=(1.f-gF*gF)/(4.f*3.14159f*powf(fmaxf(denomF,1e-4f),1.5f));
                    float phaseB=(1.f-gB*gB)/(4.f*3.14159f*powf(fmaxf(denomB,1e-4f),1.5f));
                    phase=vol.lobeMix*phaseF+(1.f-vol.lobeMix)*phaseB;
                }

                float shadowTrans=1.f;
                if (!params.previewMode && vol.shadowSteps>0 && vol.shadowDensity>0.01f) {
                    float3 sDir=make_float3(-lDir.x,-lDir.y,-lDir.z);
                    float voxelSz = fmaxf(fmaxf(
                        (vol.bboxMax.x-vol.bboxMin.x)/vol.resX,
                        (vol.bboxMax.y-vol.bboxMin.y)/vol.resY),
                        (vol.bboxMax.z-vol.bboxMin.z)/vol.resZ);
                    float sDt = voxelSz * fmaxf(1.f, 32.f / fmaxf(1.f, float(vol.shadowSteps)));
                    unsigned int jSeed = seed + step*97u + li*53u;
                    float jitter = hashRNG(jSeed) * sDt;
                    for (int ss=0;ss<vol.shadowSteps;++ss) {
                        float sT = sDt * (ss + 0.5f) + jitter;
                        float spx=px+sDir.x*sT, spy=py+sDir.y*sT, spz=pz+sDir.z*sT;
                        float su,sv,sw;
                        worldToVolUV(vol, spx, spy, spz, su, sv, sw);
                        if(su<0||su>1||sv<0||sv>1||sw<0||sw>1) break;
                        float sd = sampleDensity(vol,su,sv,sw);
                        if (sd < 1e-5f) continue;
                        shadowTrans*=expf(-sd*vol.extinction*vol.shadowDensity*sDt);
                        if(shadowTrans<0.001f) break;
                    }
                    // Cross-volume shadows
                    if (shadowTrans > 0.01f && params.numGpuVolumes > 1) {
                        for (int ovi = 0; ovi < params.numGpuVolumes; ++ovi) {
                            if (&params.gpuVolumes[ovi] == &vol) continue;
                            const GPUVolume& ovol = params.gpuVolumes[ovi];
                            if (!ovol.densityTex) continue;
                            float3 oInv=make_float3(1.f/(fabsf(sDir.x)>1e-8f?sDir.x:1e-8f),1.f/(fabsf(sDir.y)>1e-8f?sDir.y:1e-8f),1.f/(fabsf(sDir.z)>1e-8f?sDir.z:1e-8f));
                            float3 ot0=make_float3((ovol.bboxMin.x-px)*oInv.x,(ovol.bboxMin.y-py)*oInv.y,(ovol.bboxMin.z-pz)*oInv.z);
                            float3 ot1=make_float3((ovol.bboxMax.x-px)*oInv.x,(ovol.bboxMax.y-py)*oInv.y,(ovol.bboxMax.z-pz)*oInv.z);
                            float otNear=fmaxf(fmaxf(fminf(ot0.x,ot1.x),fminf(ot0.y,ot1.y)),fminf(ot0.z,ot1.z));
                            float otFar=fminf(fminf(fmaxf(ot0.x,ot1.x),fmaxf(ot0.y,ot1.y)),fmaxf(ot0.z,ot1.z));
                            otNear=fmaxf(otNear,0.f);
                            if (otNear >= otFar) continue;
                            float oVoxel=fmaxf(fmaxf((ovol.bboxMax.x-ovol.bboxMin.x)/ovol.resX,(ovol.bboxMax.y-ovol.bboxMin.y)/ovol.resY),(ovol.bboxMax.z-ovol.bboxMin.z)/ovol.resZ);
                            float oDt=oVoxel*2.f;
                            int oSteps=min(16,int((otFar-otNear)/oDt)+1);
                            for (int os=0;os<oSteps;++os) {
                                float ot=otNear+os*oDt;
                                float osx=px+sDir.x*ot,osy=py+sDir.y*ot,osz=pz+sDir.z*ot;
                                float osu,osv,osw;
                                worldToVolUV(ovol,osx,osy,osz,osu,osv,osw);
                                if(osu<0||osu>1||osv<0||osv>1||osw<0||osw>1) continue;
                                float od=sampleDensity(ovol,osu,osv,osw);
                                if(od<1e-5f) continue;
                                shadowTrans*=expf(-od*ovol.extinction*oDt);
                                if(shadowTrans<0.001f) break;
                            }
                            if(shadowTrans<0.001f) break;
                        }
                    }
                }

                // Geometry occlusion: one optixTrace per light (not per step)
                // Check if opaque geometry blocks light reaching this volume point
                if (!params.previewMode && shadowTrans > 0.01f && params.hasRealGeometry) {
                    float3 shadowDir = make_float3(-lDir.x, -lDir.y, -lDir.z);
                    float3 shadowOrig = make_float3(px+shadowDir.x*0.01f, py+shadowDir.y*0.01f, pz+shadowDir.z*0.01f);
                    unsigned int gp0=0,gp1=0,gp2=0,gp3=__float_as_uint(1e30f),gp4=0,gp5=0,gp6=0;
                    optixTrace(params.traversable, shadowOrig, shadowDir,
                               1e-3f, 1e30f, 0.f, OptixVisibilityMask(0xFF),
                               OPTIX_RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                               0, 1, 0, gp0,gp1,gp2,gp3,gp4,gp5,gp6);
                    if (gp4 != 0u) {
                        int gMatId = int(gp4) - 1;
                        if (gMatId >= 0 && gMatId < int(params.materialCount)) {
                            const GPUMaterial& gMat = params.materials[gMatId];
                            if (gMat.opacity > 0.99f || gMat.metallic > 0.5f)
                                shadowTrans = 0.f;
                            else
                                shadowTrans *= (1.f - gMat.opacity);
                        } else {
                            shadowTrans = 0.f;
                        }
                    }
                }

                float scatW=vol.scattering*density*phase*shadowTrans*powder;
                stepRGB.x+=lCol.x*vol.scatterColor.x*scatW;
                stepRGB.y+=lCol.y*vol.scatterColor.y*scatW;
                stepRGB.z+=lCol.z*vol.scatterColor.z*scatW;
            }

            // HDRI virtual lights — directional or sphere for soft shadows
            for (int vli = 0; vli < params.numVirtualLights; ++vli) {
                float3 vlDir = params.gpuVirtualLights[vli].dir;
                float3 vlCol = params.gpuVirtualLights[vli].color;
                float vlRadius = params.gpuVirtualLights[vli].radius;

                float3 lDir;
                if (vlRadius > 0.01f) {
                    // Sphere light at distance 500 for soft shadows
                    float dist = 500.f;
                    float3 lightPos = make_float3(-vlDir.x*dist, -vlDir.y*dist, -vlDir.z*dist);
                    // Jitter on sphere surface using step index as seed
                    unsigned int vlSeed = seed + step * 131u + vli * 37u;
                    float ju1 = hashRNG(vlSeed); vlSeed += 0x9e3779b9u;
                    float ju2 = hashRNG(vlSeed);
                    float jTheta = 6.28318f * ju1;
                    float jPhi = acosf(1.f - 2.f * ju2);
                    float jsp = sinf(jPhi);
                    lightPos.x += vlRadius * jsp * cosf(jTheta);
                    lightPos.y += vlRadius * jsp * sinf(jTheta);
                    lightPos.z += vlRadius * cosf(jPhi);
                    // Direction from volume point to jittered light position
                    lDir = make_float3(px-lightPos.x, py-lightPos.y, pz-lightPos.z);
                    float dl = sqrtf(lDir.x*lDir.x+lDir.y*lDir.y+lDir.z*lDir.z);
                    if (dl>1e-6f) { lDir.x/=dl; lDir.y/=dl; lDir.z/=dl; }
                } else {
                    // Hard shadow: distant light
                    lDir = make_float3(-vlDir.x, -vlDir.y, -vlDir.z);
                }
                float cosTheta = -(rd.x*lDir.x+rd.y*lDir.y+rd.z*lDir.z);

                float phase;
                if (vol.phaseMode==1) {
                    float d=vol.mieDropletD;
                    float g=0.85f*(1.f-expf(-d*0.8f));
                    float g2=g*g;
                    float num=(1.f-g2)*(1.f+cosTheta*cosTheta);
                    float denom=(2.f+g2)*powf(fmaxf(1.f+g2-2.f*g*cosTheta,1e-4f),1.5f);
                    phase=(3.f/(8.f*3.14159f))*num/denom;
                } else {
                    float gF=vol.gForward, gB=vol.gBackward;
                    float denomF=1.f+gF*gF-2.f*gF*cosTheta, denomB=1.f+gB*gB-2.f*gB*cosTheta;
                    float phaseF=(1.f-gF*gF)/(4.f*3.14159f*powf(fmaxf(denomF,1e-4f),1.5f));
                    float phaseB=(1.f-gB*gB)/(4.f*3.14159f*powf(fmaxf(denomB,1e-4f),1.5f));
                    phase=vol.lobeMix*phaseF+(1.f-vol.lobeMix)*phaseB;
                }

                // Shadow ray for virtual light
                float shadowTrans=1.f;
                if (!params.previewMode && vol.shadowSteps>0 && vol.shadowDensity>0.01f) {
                    float3 sDir=make_float3(-lDir.x,-lDir.y,-lDir.z);
                    float voxelSz = fmaxf(fmaxf(
                        (vol.bboxMax.x-vol.bboxMin.x)/vol.resX,
                        (vol.bboxMax.y-vol.bboxMin.y)/vol.resY),
                        (vol.bboxMax.z-vol.bboxMin.z)/vol.resZ);
                    float sDt = voxelSz * fmaxf(1.f, 32.f / fmaxf(1.f, float(vol.shadowSteps)));
                    unsigned int jSeed = seed + step*97u + (vli+100u)*53u;
                    float jitter = hashRNG(jSeed) * sDt;
                    for (int ss=0;ss<vol.shadowSteps;++ss) {
                        float sT = sDt * (ss + 0.5f) + jitter;
                        float spx=px+sDir.x*sT, spy=py+sDir.y*sT, spz=pz+sDir.z*sT;
                        float su,sv,sw;
                        worldToVolUV(vol, spx, spy, spz, su, sv, sw);
                        if(su<0||su>1||sv<0||sv>1||sw<0||sw>1) break;
                        float sd = sampleDensity(vol,su,sv,sw);
                        if (sd < 1e-5f) continue;
                        shadowTrans*=expf(-sd*vol.extinction*vol.shadowDensity*sDt);
                        if(shadowTrans<0.001f) break;
                    }
                    // Cross-volume shadows for virtual lights
                    if (shadowTrans > 0.01f && params.numGpuVolumes > 1) {
                        for (int ovi = 0; ovi < params.numGpuVolumes; ++ovi) {
                            if (&params.gpuVolumes[ovi] == &vol) continue;
                            const GPUVolume& ovol = params.gpuVolumes[ovi];
                            if (!ovol.densityTex) continue;
                            float3 oInv=make_float3(1.f/(fabsf(sDir.x)>1e-8f?sDir.x:1e-8f),1.f/(fabsf(sDir.y)>1e-8f?sDir.y:1e-8f),1.f/(fabsf(sDir.z)>1e-8f?sDir.z:1e-8f));
                            float3 ot0=make_float3((ovol.bboxMin.x-px)*oInv.x,(ovol.bboxMin.y-py)*oInv.y,(ovol.bboxMin.z-pz)*oInv.z);
                            float3 ot1=make_float3((ovol.bboxMax.x-px)*oInv.x,(ovol.bboxMax.y-py)*oInv.y,(ovol.bboxMax.z-pz)*oInv.z);
                            float otNear=fmaxf(fmaxf(fminf(ot0.x,ot1.x),fminf(ot0.y,ot1.y)),fminf(ot0.z,ot1.z));
                            float otFar=fminf(fminf(fmaxf(ot0.x,ot1.x),fmaxf(ot0.y,ot1.y)),fmaxf(ot0.z,ot1.z));
                            otNear=fmaxf(otNear,0.f);
                            if (otNear >= otFar) continue;
                            float oVoxel=fmaxf(fmaxf((ovol.bboxMax.x-ovol.bboxMin.x)/ovol.resX,(ovol.bboxMax.y-ovol.bboxMin.y)/ovol.resY),(ovol.bboxMax.z-ovol.bboxMin.z)/ovol.resZ);
                            float oDt=oVoxel*2.f;
                            int oSteps=min(16,int((otFar-otNear)/oDt)+1);
                            for (int os=0;os<oSteps;++os) {
                                float ot=otNear+os*oDt;
                                float osx=px+sDir.x*ot,osy=py+sDir.y*ot,osz=pz+sDir.z*ot;
                                float osu,osv,osw;
                                worldToVolUV(ovol,osx,osy,osz,osu,osv,osw);
                                if(osu<0||osu>1||osv<0||osv>1||osw<0||osw>1) continue;
                                float od=sampleDensity(ovol,osu,osv,osw);
                                if(od<1e-5f) continue;
                                shadowTrans*=expf(-od*ovol.extinction*oDt);
                                if(shadowTrans<0.001f) break;
                            }
                            if(shadowTrans<0.001f) break;
                        }
                    }
                }

                // Geometry occlusion for virtual light
                if (!params.previewMode && shadowTrans > 0.01f && params.hasRealGeometry) {
                    float3 shadowDir = make_float3(-lDir.x, -lDir.y, -lDir.z);
                    float3 shadowOrig = make_float3(px+shadowDir.x*0.01f, py+shadowDir.y*0.01f, pz+shadowDir.z*0.01f);
                    unsigned int gp0=0,gp1=0,gp2=0,gp3=__float_as_uint(1e30f),gp4=0,gp5=0,gp6=0;
                    optixTrace(params.traversable, shadowOrig, shadowDir,
                               1e-3f, 1e30f, 0.f, OptixVisibilityMask(0xFF),
                               OPTIX_RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                               0, 1, 0, gp0,gp1,gp2,gp3,gp4,gp5,gp6);
                    if (gp4 != 0u) {
                        int gMatId = int(gp4) - 1;
                        if (gMatId >= 0 && gMatId < int(params.materialCount)) {
                            const GPUMaterial& gMat = params.materials[gMatId];
                            if (gMat.opacity > 0.99f || gMat.metallic > 0.5f)
                                shadowTrans = 0.f;
                            else
                                shadowTrans *= (1.f - gMat.opacity);
                        } else {
                            shadowTrans = 0.f;
                        }
                    }
                }

                float vScatW=vol.scattering*density*phase*shadowTrans*powder;
                stepRGB.x+=vlCol.x*vol.scatterColor.x*vScatW;
                stepRGB.y+=vlCol.y*vol.scatterColor.y*vScatW;
                stepRGB.z+=vlCol.z*vol.scatterColor.z*vScatW;
            }

            // Dome ambient — SH or flat average (only if dome has actual intensity)
            for (unsigned li=0; li<params.lightCount; ++li) {
                if (params.lights[li].type==3) {
                    float domeI = params.lights[li].intensity;
                    if (domeI < 0.001f) break;  // dome disabled
                    float dW=vol.scattering*density;
                    float3 domeCol;
                    if (params.hasEnvSH) {
                        // SH L0+L1 evaluated at negative ray direction
                        float3 evalDir = make_float3(-rd.x, -rd.y, -rd.z);
                        domeCol.x = params.envSH[0].x*0.2821f + params.envSH[1].x*0.4886f*evalDir.y
                                  + params.envSH[2].x*0.4886f*evalDir.z + params.envSH[3].x*0.4886f*evalDir.x;
                        domeCol.y = params.envSH[0].y*0.2821f + params.envSH[1].y*0.4886f*evalDir.y
                                  + params.envSH[2].y*0.4886f*evalDir.z + params.envSH[3].y*0.4886f*evalDir.x;
                        domeCol.z = params.envSH[0].z*0.2821f + params.envSH[1].z*0.4886f*evalDir.y
                                  + params.envSH[2].z*0.4886f*evalDir.z + params.envSH[3].z*0.4886f*evalDir.x;
                        domeCol.x = fmaxf(0.f, domeCol.x);
                        domeCol.y = fmaxf(0.f, domeCol.y);
                        domeCol.z = fmaxf(0.f, domeCol.z);
                    } else {
                        domeCol = make_float3(
                            params.lights[li].color.x*params.lights[li].intensity,
                            params.lights[li].color.y*params.lights[li].intensity,
                            params.lights[li].color.z*params.lights[li].intensity);
                    }
                    stepRGB.x+=domeCol.x*dW;
                    stepRGB.y+=domeCol.y*dW;
                    stepRGB.z+=domeCol.z*dW;
                    break;
                }
            }
            // Phase 14: analytical MS approximation (Wrenninge 2015)
            // Only applies when there's actual incident light (not self-illuminating)
            if (vol.msApprox && vol.scattering > 0.01f && (stepRGB.x + stepRGB.y + stepRGB.z) > 0.001f) {
                float albedo = vol.scattering / fmaxf(vol.extinction, 0.01f);
                float msB = albedo / fmaxf(1.f - albedo * vol.gForward * vol.lobeMix, 0.01f);
                // Scale MS by ratio of actual light to expected light (so it's proportional)
                float lightScale = fminf((stepRGB.x + stepRGB.y + stepRGB.z) * 0.5f, 1.f);
                stepRGB.x += vol.msTint.x * density * vol.scattering * msB * 0.3f * lightScale;
                stepRGB.y += vol.msTint.y * density * vol.scattering * msB * 0.3f * lightScale;
                stepRGB.z += vol.msTint.z * density * vol.scattering * msB * 0.3f * lightScale;
            }
            stepRGB.x*=vol.intensity; stepRGB.y*=vol.intensity; stepRGB.z*=vol.intensity;
        }

        outRGB.x += outTransmittance * absorption * stepRGB.x;
        outRGB.y += outTransmittance * absorption * stepRGB.y;
        outRGB.z += outTransmittance * absorption * stepRGB.z;

        // Emission
        if (rmode==4||rmode==5||vol.emissionIntensity>0.01f) {
            float3 emRGB=make_float3(0.f,0.f,0.f);
            float temp2=sampleTemp(vol,u,v,w)*vol.tempMix;
            if (temp2>vol.tempMin) {
                float tN2=fminf((temp2-vol.tempMin)/(vol.tempMax-vol.tempMin+1e-6f),1.f);
                float T2=vol.tempMin+tN2*(vol.tempMax-vol.tempMin);
                float t100=T2/100.f; float cr=1,cg=1,cb=1;
                if(T2<=6600.f){cr=1;cg=fmaxf(0.f,fminf(0.39f*logf(t100)-0.63f,1.f));
                    cb=fmaxf(0.f,fminf(0.54f*logf(t100-10.f)-1.19f,1.f));}
                else{cr=fmaxf(0.f,fminf(1.29f*powf(t100-60.f,-0.13f),1.f));
                    cg=fmaxf(0.f,fminf(1.13f*powf(t100-60.f,-0.07f),1.f));cb=1;}
                emRGB=make_float3(cr*vol.emissionIntensity*tN2,
                                  cg*vol.emissionIntensity*tN2,
                                  cb*vol.emissionIntensity*tN2);
            }
            // Phase 17: flame grid with artist temp range
            if(flameVal>0.01f){
                float T3=vol.flameTempMin+flameVal*(vol.flameTempMax-vol.flameTempMin);
                float t100=T3/100.f;
                float cr=1,cg=fmaxf(0.f,fminf(0.39f*logf(t100)-0.63f,1.f)),
                      cb=fmaxf(0.f,fminf(0.54f*logf(t100-10.f)-1.19f,1.f));
                emRGB.x+=cr*vol.flameIntensity*flameVal;
                emRGB.y+=cg*vol.flameIntensity*flameVal;
                emRGB.z+=cb*vol.flameIntensity*flameVal;
            }
            // Phase 17: dense core glow
            if (vol.coreGlow > 0.01f && density > 0.3f) {
                float coreFrac = fminf((density - 0.3f) / 0.7f, 1.f);
                float T4=vol.coreTemp, t100=T4/100.f;
                float cr=1,cg=fmaxf(0.f,fminf(0.39f*logf(t100)-0.63f,1.f)),
                      cb=fmaxf(0.f,fminf(0.54f*logf(t100-10.f)-1.19f,1.f));
                float coreE = vol.coreGlow*coreFrac*coreFrac;
                emRGB.x+=cr*coreE; emRGB.y+=cg*coreE; emRGB.z+=cb*coreE;
            }
            // Phase 17: Cherenkov radiation — blue glow at high density
            if (vol.cherenkov && density > vol.cherenkovThreshold) {
                float chFrac = fminf((density-vol.cherenkovThreshold)/(1.f-vol.cherenkovThreshold+1e-6f), 1.f);
                float chE = vol.cherenkovStrength*chFrac*chFrac;
                emRGB.x+=0.15f*chE; emRGB.y+=0.4f*chE; emRGB.z+=1.f*chE;
            }
            float emW=outTransmittance*density*dt*vol.intensity;
            outRGB.x+=emRGB.x*emW; outRGB.y+=emRGB.y*emW; outRGB.z+=emRGB.z*emW;
        }

        outTransmittance *= stepTrans;
        if (outTransmittance < 0.001f) break;
    }
}

// Multi-volume compositing wrapper - matches CPU logic (Phase 13)
static __forceinline__ __device__ void marchVolume(
    float3 ro, float3 rd, float surfaceT, float lambda, unsigned int seed,
    float3& outRGB, float& outTransmittance)
{
    outRGB = make_float3(0.f, 0.f, 0.f);
    outTransmittance = 1.f;

    for (int vi = 0; vi < params.numGpuVolumes; ++vi) {
        const spectral_gpu::GPUVolume& vol = params.gpuVolumes[vi];
        if (!vol.densityTex) continue;

        float3 volRGB = make_float3(0.f, 0.f, 0.f);
        float volTrans = 1.f;
        marchSingleVolume(vol, ro, rd, surfaceT, lambda, seed + vi * 31u,
                          volRGB, volTrans);

        // Composite: accumulate scatter, multiply transmittance
        outRGB.x += outTransmittance * volRGB.x;
        outRGB.y += outTransmittance * volRGB.y;
        outRGB.z += outTransmittance * volRGB.z;
        outTransmittance *= volTrans;
    }
}

// Distance to first dense voxel (for depth/deep output)
// Lightweight march — no shading, just finds where density > threshold
static __forceinline__ __device__ float volumeFirstDenseT(float3 ro, float3 rdRaw)
{
    float nearest = 1e30f;
    float rdLen = sqrtf(rdRaw.x*rdRaw.x + rdRaw.y*rdRaw.y + rdRaw.z*rdRaw.z);
    if (rdLen < 1e-8f) return nearest;
    float3 rd = make_float3(rdRaw.x/rdLen, rdRaw.y/rdLen, rdRaw.z/rdLen);

    for (int vi = 0; vi < params.numGpuVolumes; ++vi) {
        const spectral_gpu::GPUVolume& vol = params.gpuVolumes[vi];
        if (!vol.densityTex) continue;
        float3 invDir = make_float3(
            1.f/(fabsf(rd.x)>1e-8f?rd.x:1e-8f),
            1.f/(fabsf(rd.y)>1e-8f?rd.y:1e-8f),
            1.f/(fabsf(rd.z)>1e-8f?rd.z:1e-8f));
        float3 t0=make_float3((vol.bboxMin.x-ro.x)*invDir.x,(vol.bboxMin.y-ro.y)*invDir.y,(vol.bboxMin.z-ro.z)*invDir.z);
        float3 t1=make_float3((vol.bboxMax.x-ro.x)*invDir.x,(vol.bboxMax.y-ro.y)*invDir.y,(vol.bboxMax.z-ro.z)*invDir.z);
        float tNear=fmaxf(fmaxf(fminf(t0.x,t1.x),fminf(t0.y,t1.y)),fminf(t0.z,t1.z));
        float tFar=fminf(fminf(fmaxf(t0.x,t1.x),fmaxf(t0.y,t1.y)),fmaxf(t0.z,t1.z));
        tNear=fmaxf(tNear,0.f);
        if (tNear >= tFar) continue;

        // March with coarse steps to find first dense voxel
        float3 bSize=make_float3(vol.bboxMax.x-vol.bboxMin.x,vol.bboxMax.y-vol.bboxMin.y,vol.bboxMax.z-vol.bboxMin.z);
        float voxelSize=fmaxf(fmaxf(bSize.x/vol.resX,bSize.y/vol.resY),bSize.z/vol.resZ);
        float dt = voxelSize * 2.f;  // coarse steps for speed
        int maxSteps = min(128, int((tFar-tNear)/dt)+1);
        for (int step=0; step<maxSteps; ++step) {
            float t = tNear + step * dt;
            if (t >= tFar) break;
            float px=ro.x+rd.x*t, py=ro.y+rd.y*t, pz=ro.z+rd.z*t;
            float u,v,w;
            worldToVolUV(vol, px, py, pz, u, v, w);
            float d = sampleDensity(vol, u, v, w);
            if (d > 0.01f) {
                // Found dense voxel — refine with one half-step back
                float tRefined = (step > 0) ? t - dt*0.5f : t;
                if (tRefined < nearest) nearest = tRefined;
                break;
            }
        }
    }
    return nearest;
}

// ---------------------------------------------------------------------------
// __raygen__spectral
// ---------------------------------------------------------------------------
extern "C" __global__ void __raygen__spectral()
{
    const uint3 idx = optixGetLaunchIndex();
    const uint3 dim = optixGetLaunchDimensions();
    const unsigned int px=idx.x, py_local=idx.y;
    const unsigned int py = py_local + params.yOffset;  // absolute Y in full image
    const unsigned int pixIdx = py*params.width+px;
    const int spp = params.spp;
    const float W=float(params.width), H=float(params.height);

    if (spp <= 1) {
        // Normal-as-colour (or volume density preview for spp=1)
        float3 origin, dir;
        unsigned int dofSeed = pixIdx * 7919u;
        makeRay(px+0.5f, py+0.5f, W, H, origin, dir, dofSeed);
        unsigned int p0=0,p1=0,p2=0,p3=__float_as_uint(1e30f),p4=0,p5=0,p6=0;

        // Hit Object API: traverse → reorder → invoke (SER for volume coherence)
        optixTraverse(params.traversable, origin, dir, 1e-4f,1e30f,0.f,
                      OptixVisibilityMask(0xFF), OPTIX_RAY_FLAG_NONE, 0,1,0);
        optixReorder();
        optixInvoke(p0,p1,p2,p3,p4,p5,p6);

        float r = __uint_as_float(p0);
        float g = __uint_as_float(p1);
        float b = __uint_as_float(p2);
        bool spp1Hit = (p4 > 0u);

        // Background: dome light color on miss (skip when volume for comp)
        if (!spp1Hit) {
            r = 0.f; g = 0.f; b = 0.f;
            if (params.numGpuVolumes == 0) {
                for (unsigned int li = 0; li < params.lightCount; ++li) {
                    const GPULight& L = params.lights[li];
                    if (L.type == 3) {
                        r += L.color.x * L.intensity;
                        g += L.color.y * L.intensity;
                        b += L.color.z * L.intensity;
                    }
                }
            }
        }

        // Volume overlay for spp=1
        float spp1Alpha = spp1Hit ? 1.f : 0.f;
        if (params.numGpuVolumes > 0) {
            float surfT = spp1Hit ? __uint_as_float(p3) : 1e30f;
            float3 volRGB = make_float3(0.f,0.f,0.f); float volTrans = 1.f;
            marchVolume(origin, dir, surfT, 550.f, dofSeed + 200u, volRGB, volTrans);
            r = volRGB.x + volTrans * r;
            g = volRGB.y + volTrans * g;
            b = volRGB.z + volTrans * b;
            spp1Alpha = 1.f - volTrans * (1.f - spp1Alpha);
        }

        params.framebuffer[pixIdx] = make_float4(r, g, b, spp1Alpha);
        if (params.depthbuffer) {
            float d = __uint_as_float(p3);
            // Volume depth fallback: if no surface hit but volume had contribution
            if (d >= 1e29f && params.numGpuVolumes > 0 && spp1Alpha > 0.001f) {
                d = volumeFirstDenseT(origin, dir);
            }
            params.depthbuffer[pixIdx] = d;
        }
    } else {
        // Spectral with BSDF + lights + bounces
        float X=0.f, Y=0.f, Z=0.f;
        float alphaAccum = 0.f;
        float minDepth = 1e30f;
        const int maxBounces = params.maxBounces;

        for (int s = 0; s < spp; ++s) {
            unsigned int seed = pixIdx*1031u + s*6571u;
            float jx = hashRNG(seed); float jy = hashRNG(seed+1u);
            float wu = (float(s)+hashRNG(seed+2u))/float(spp);

            // R2 quasi-random override
            if (params.blueNoise) {
                unsigned int pixSeed = py * dim.x + px;
                float r2off1 = hashRNG(pixSeed * 2u);
                float r2off2 = hashRNG(pixSeed * 2u + 1u);
                jx = fmodf(r2off1 + float(s) * 0.7548776662f, 1.f);
                jy = fmodf(r2off2 + float(s) * 0.5698402909f, 1.f);
                wu = fmodf(hashRNG(pixSeed * 3u) + float(s) * 0.7548776662f, 1.f);
            }
            float lambda = 380.f + wu*400.f;

            float3 origin, dir;
            makeRay(px+jx, py+jy, W, H, origin, dir, seed + 50u);

            // Primary ray — Hit Object API for SER
            unsigned int p0=0,p1=0,p2=0,p3=__float_as_uint(1e30f),p4=0,p5=0,p6=0;
            optixTraverse(params.traversable, origin, dir, 1e-4f,1e30f,0.f,
                          OptixVisibilityMask(0xFF), OPTIX_RAY_FLAG_NONE, 0,1,0);
            optixReorder();
            optixInvoke(p0,p1,p2,p3,p4,p5,p6);

            float depth = __uint_as_float(p3);
            bool isHit = (p4 > 0u);

            float radiance = 0.f;

            if (isHit) {
                int matId = int(p4)-1;
                if (matId<0||matId>=int(params.materialCount)) matId=0;
                GPUMaterial mat = params.materials[matId];

                // Resolve texture at hit UV with texture blend
                if (mat.baseColorTexId >= 0 && mat.textureBlend > 0.f) {
                    float2 hitUV = make_float2(__uint_as_float(p5), __uint_as_float(p6));
                    float3 texCol = sampleTextureGPU(mat.baseColorTexId, hitUV);
                    float b = mat.textureBlend;
                    mat.baseColor = make_float3(
                        mat.baseColor.x*(1.f-b) + texCol.x*b,
                        mat.baseColor.y*(1.f-b) + texCol.y*b,
                        mat.baseColor.z*(1.f-b) + texCol.z*b);
                }

                float3 N = normalize3(make_float3(
                    __uint_as_float(p0),__uint_as_float(p1),__uint_as_float(p2)));

                // GPU bump mapping
                if (mat.bumpMapTexId >= 0 && mat.bumpStrength > 0.f) {
                    float2 hitUV = make_float2(__uint_as_float(p5), __uint_as_float(p6));
                    float eps = 0.002f;
                    float hC = sampleTextureGPU(mat.bumpMapTexId, hitUV).x;
                    float hR = sampleTextureGPU(mat.bumpMapTexId, make_float2(hitUV.x+eps, hitUV.y)).x;
                    float hU = sampleTextureGPU(mat.bumpMapTexId, make_float2(hitUV.x, hitUV.y+eps)).x;
                    float dhdU = (hR - hC) / eps * mat.bumpStrength;
                    float dhdV = (hU - hC) / eps * mat.bumpStrength;
                    // Build tangent frame from normal
                    float3 up = (fabsf(N.y) < 0.999f) ? make_float3(0,1,0) : make_float3(1,0,0);
                    float3 T = normalize3(cross3(up, N));
                    float3 B = cross3(N, T);
                    N = normalize3(make_float3(
                        N.x - dhdU*T.x - dhdV*B.x,
                        N.y - dhdU*T.y - dhdV*B.y,
                        N.z - dhdU*T.z - dhdV*B.z));
                }

                float3 V = normalize3(neg3(dir));
                if (dot3raw(N,V)<0.f) N=neg3(N);

                float3 hitPos = make_float3(origin.x+depth*dir.x, origin.y+depth*dir.y, origin.z+depth*dir.z);
                if (depth < minDepth) minDepth = depth;

                // Direct lighting at primary hit
                unsigned int shadowSeed = seed + 50u;
                radiance = shadeHit(mat, N, V, hitPos, lambda, shadowSeed);

                // Bounce rays with refraction support
                float throughput = 1.f;
                float3 bN=N, bV=V, bOrigin=hitPos;
                const GPUMaterial* bMat = &mat;
                GPUMaterial resolvedMat = mat;
                unsigned int bSeed = seed + 100u;
                bool isEntering = true;
                bool insideVolume = false;
                GPUMaterial volumeMat;
                float pathMinRough = 0.f;  // path regularization

                for (int bounce = 0; bounce < maxBounces; ++bounce) {
                    if (bounce >= 1) {
                        float rrProb = fminf(0.95f, throughput);
                        if (hashRNG(bSeed++) > rrProb) break;
                        throughput /= rrProb;
                    }

                    // Path regularization
                    GPUMaterial regMat = *bMat;
                    if (pathMinRough > regMat.roughness) regMat.roughness = pathMinRough;

                    float bounceThroughput;
                    bool bTransmitted = false;
                    float3 bounceDir = sampleBounce(regMat, bN, bV, lambda,
                        hashRNG(bSeed++), hashRNG(bSeed++), bounceThroughput, bTransmitted, isEntering);
                    if (bounceThroughput <= 0.f) break;
                    throughput *= bounceThroughput;

                    // Volume tracking on transmission
                    if (bTransmitted) {
                        if (bMat->roughness > 0.05f)
                            pathMinRough = fmaxf(pathMinRough, bMat->roughness * 0.5f);
                        if (isEntering && bMat->absorptionDensity > 0.f) {
                            insideVolume = true; volumeMat = *bMat;
                        } else if (!isEntering) {
                            insideVolume = false;
                        }
                    }

                    // Offset: along -N for transmission, +N for reflection
                    float bOff = bTransmitted ? -0.1f : 0.01f;
                    float3 bOrig = make_float3(bOrigin.x+bN.x*bOff, bOrigin.y+bN.y*bOff, bOrigin.z+bN.z*bOff);
                    unsigned int bp0=0,bp1=0,bp2=0,bp3=__float_as_uint(1e30f),bp4=0,bp5=0,bp6=0;
                    optixTrace(params.traversable, bOrig, bounceDir, 1e-4f,1e30f,0.f,
                               OptixVisibilityMask(0xFF), OPTIX_RAY_FLAG_NONE, 0,1,0, bp0,bp1,bp2,bp3,bp4,bp5,bp6);

                    if (bp4 == 0u) {
                        // Bounce miss — march through volumes before adding dome
                        if (params.numGpuVolumes > 0) {
                            float3 volRGB; float volTrans;
                            marchVolume(bOrig, bounceDir, 1e30f, lambda, bSeed + bounce*97u, volRGB, volTrans);
                            // Convert RGB to spectral: weight by wavelength
                            float volSpec = (lambda < 500.f) ? volRGB.z :
                                           (lambda < 580.f) ? volRGB.y : volRGB.x;
                            radiance += throughput * volSpec;
                            throughput *= volTrans;
                        }
                        // Dome contribution (MIS weight 0.5, other half in direct)
                        for (unsigned int li = 0; li < params.lightCount; ++li) {
                            const GPULight& domeL = params.lights[li];
                            if (domeL.type != 3) continue;
                            radiance += throughput * lightEmission(domeL, lambda) * 0.5f;
                        }
                        break;
                    }

                    // Bounce hit — march through volumes between origin and hit
                    float bDepth = __uint_as_float(bp3);
                    if (params.numGpuVolumes > 0) {
                        float3 volRGB; float volTrans;
                        marchVolume(bOrig, bounceDir, bDepth, lambda, bSeed + bounce*97u, volRGB, volTrans);
                        float volSpec = (lambda < 500.f) ? volRGB.z :
                                       (lambda < 580.f) ? volRGB.y : volRGB.x;
                        radiance += throughput * volSpec;
                        throughput *= volTrans;
                    }
                    int bMatId = int(bp4)-1;
                    if (bMatId<0||bMatId>=int(params.materialCount)) bMatId=0;
                    resolvedMat = params.materials[bMatId];  // copy for texture resolution
                    if (resolvedMat.baseColorTexId >= 0 && resolvedMat.textureBlend > 0.f) {
                        float2 bUV = make_float2(__uint_as_float(bp5), __uint_as_float(bp6));
                        float3 texCol = sampleTextureGPU(resolvedMat.baseColorTexId, bUV);
                        float b = resolvedMat.textureBlend;
                        resolvedMat.baseColor = make_float3(
                            resolvedMat.baseColor.x*(1.f-b) + texCol.x*b,
                            resolvedMat.baseColor.y*(1.f-b) + texCol.y*b,
                            resolvedMat.baseColor.z*(1.f-b) + texCol.z*b);
                    }
                    bMat = &resolvedMat;

                    float3 rawN = normalize3(make_float3(
                        __uint_as_float(bp0),__uint_as_float(bp1),__uint_as_float(bp2)));
                    bV = normalize3(neg3(bounceDir));
                    float bNdV = dot3raw(rawN, bV);
                    isEntering = (bNdV > 0.f);

                    // Beer-Lambert: absorption for distance traveled inside volume
                    if (insideVolume && bDepth > 0.f && volumeMat.absorptionDensity > 0.f) {
                        float rB = expf(-((lambda-630.f)*(lambda-630.f))/(2.f*30.f*30.f));
                        float gB = expf(-((lambda-532.f)*(lambda-532.f))/(2.f*30.f*30.f));
                        float bB = expf(-((lambda-460.f)*(lambda-460.f))/(2.f*25.f*25.f));
                        float colAtL = volumeMat.absorptionColor.x*rB + volumeMat.absorptionColor.y*gB + volumeMat.absorptionColor.z*bB;
                        colAtL = fmaxf(0.001f, fminf(1.f, colAtL));
                        float sigma = -logf(colAtL) * volumeMat.absorptionDensity;
                        throughput *= expf(-sigma * bDepth);
                        if (throughput < 1e-6f) break;
                    }

                    bN = (bNdV < 0.f) ? neg3(rawN) : rawN;
                    bOrigin = make_float3(bOrig.x+bounceDir.x*bDepth, bOrig.y+bounceDir.y*bDepth, bOrig.z+bounceDir.z*bDepth);

                    // Skip direct lighting inside transparent objects
                    bool insideGlass = (!isEntering && bMat->opacity < 0.99f);
                    if (!insideGlass)
                        radiance += throughput * shadeHit(*bMat, bN, bV, bOrigin, lambda, bSeed);
                }
            } else {
                // Miss — dome background only when no volume (for comp)
                radiance = 0.f;
                if (params.numGpuVolumes == 0) {
                    for (unsigned int li = 0; li < params.lightCount; ++li) {
                        const GPULight& L = params.lights[li];
                        if (L.type == 3) {
                            float lumR = expf(-((lambda-630.f)*(lambda-630.f))/(2.f*30.f*30.f));
                            float lumG = expf(-((lambda-532.f)*(lambda-532.f))/(2.f*30.f*30.f));
                            float lumB = expf(-((lambda-460.f)*(lambda-460.f))/(2.f*25.f*25.f));
                            radiance += (L.color.x*L.intensity*lumR + L.color.y*L.intensity*lumG + L.color.z*L.intensity*lumB);
                        }
                    }
                }
            }

            // GPU volume ray marching — gated by volumeSpp
            float sampleAlpha = isHit ? 1.f : 0.f;
            float vx=0.f, vy=0.f, vz=0.f;
            if (params.numGpuVolumes > 0 && s < params.volumeSpp) {
                float surfT = isHit ? depth : 1e30f;
                float3 volRGB = make_float3(0.f,0.f,0.f); float volTrans = 1.f;
                marchVolume(origin, dir, surfT, lambda, seed + 200u, volRGB, volTrans);
                // Scale volume contribution to compensate for fewer samples
                float volScale = float(spp) / float(params.volumeSpp);
                // Volume RGB → XYZ directly (sRGB D65 matrix)
                vx = (0.4124f*volRGB.x + 0.3576f*volRGB.y + 0.1805f*volRGB.z) * volScale;
                vy = (0.2126f*volRGB.x + 0.7152f*volRGB.y + 0.0722f*volRGB.z) * volScale;
                vz = (0.0193f*volRGB.x + 0.1192f*volRGB.y + 0.9505f*volRGB.z) * volScale;
                radiance *= volTrans;
                sampleAlpha = 1.f - volTrans * (1.f - sampleAlpha);
            }
            alphaAccum += sampleAlpha;

            float cx,cy,cz; cieXYZ(lambda,cx,cy,cz);
            float scale = 400.f/106.856895f;
            // Surface spectral + volume RGB→XYZ
            X+=radiance*cx*scale + vx; Y+=radiance*cy*scale + vy; Z+=radiance*cz*scale + vz;
        }

        float inv=1.f/float(spp); X*=inv; Y*=inv; Z*=inv;
        float r, g, b;
        if (params.colorSpace == 1) {
            // ACEScg (AP1, D60)
            r= 1.6410f*X-0.3249f*Y-0.2364f*Z;
            g=-0.6636f*X+1.6153f*Y+0.0168f*Z;
            b= 0.0117f*X-0.0083f*Y+0.9884f*Z;
        } else if (params.colorSpace == 2) {
            // ACES 2065-1 (AP0, D60)
            r= 1.0498f*X+0.0000f*Y-0.0001f*Z;
            g=-0.4959f*X+1.3735f*Y+0.0982f*Z;
            b= 0.0000f*X+0.0000f*Y+0.9913f*Z;
        } else {
            // sRGB (Rec.709, D65)
            r= 3.2406f*X-1.5372f*Y-0.4986f*Z;
            g=-0.9689f*X+1.8758f*Y+0.0415f*Z;
            b= 0.0557f*X-0.2040f*Y+1.0570f*Z;
        }
        params.framebuffer[pixIdx] = make_float4(fmaxf(0.f,r),fmaxf(0.f,g),fmaxf(0.f,b),
                                                    alphaAccum / float(spp));
        if (params.depthbuffer) {
            // Volume depth fallback for volume-only pixels
            if (minDepth >= 1e29f && params.numGpuVolumes > 0 && alphaAccum > 0.001f) {
                float3 dOrigin, dDir;
                makeRay(float(px)+0.5f, float(py)+0.5f, W, H, dOrigin, dDir, pixIdx);
                minDepth = volumeFirstDenseT(dOrigin, dDir);
            }
            params.depthbuffer[pixIdx] = minDepth;
        }
    }
}

// ---------------------------------------------------------------------------
// __closesthit__spectral — pass normal + materialId
// ---------------------------------------------------------------------------
extern "C" __global__ void __closesthit__spectral()
{
    float3 normal = getHitNormal();
    float tHit = optixGetRayTmax();
    int matId = getHitMaterialId();
    float2 hitUV = getHitUV();

    if (params.spp <= 1) {
        float3 c = shadeNormal(normal);
        optixSetPayload_0(__float_as_uint(c.x));
        optixSetPayload_1(__float_as_uint(c.y));
        optixSetPayload_2(__float_as_uint(c.z));
    } else {
        optixSetPayload_0(__float_as_uint(normal.x));
        optixSetPayload_1(__float_as_uint(normal.y));
        optixSetPayload_2(__float_as_uint(normal.z));
    }
    optixSetPayload_3(__float_as_uint(tHit));
    optixSetPayload_4(static_cast<unsigned int>(matId+1));
    optixSetPayload_5(__float_as_uint(hitUV.x));
    optixSetPayload_6(__float_as_uint(hitUV.y));
}

// ---------------------------------------------------------------------------
// __miss__spectral
// ---------------------------------------------------------------------------
extern "C" __global__ void __miss__spectral()
{
    // No sky — return black for all modes
    optixSetPayload_0(0u); optixSetPayload_1(0u); optixSetPayload_2(0u);
    optixSetPayload_3(__float_as_uint(1e30f));
    optixSetPayload_4(0u);
    optixSetPayload_5(0u);
    optixSetPayload_6(0u);
}
