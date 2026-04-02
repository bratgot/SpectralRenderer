// ---------------------------------------------------------------------------
// SpectralGPUKernel.cu — Full feature parity with CPU path
//   Explicit lights, shadow rays, bounce rays, Disney BSDF
// ---------------------------------------------------------------------------

#include <optix_device.h>
#include <cuda_runtime.h>
#include "SpectralGPUParams.h"

using namespace spectral_gpu;

extern "C" { __constant__ LaunchParams params; }

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
    float px, float py, float W, float H, float3& origin, float3& dir)
{
    float ndcX = 2.f * px / W - 1.f;
    float ndcY = -2.f * py / H + 1.f;
    float nx0,ny0,nz0,nw0; mat4mulv4(params.camera.projInverse, ndcX,ndcY,-1.f,1.f, nx0,ny0,nz0,nw0);
    float nx1,ny1,nz1,nw1; mat4mulv4(params.camera.projInverse, ndcX,ndcY, 1.f,1.f, nx1,ny1,nz1,nw1);
    float3 nearPos = make_float3(nx0/nw0, ny0/nw0, nz0/nw0);
    float3 farPos  = make_float3(nx1/nw1, ny1/nw1, nz1/nw1);
    float3 worldNear = transformPoint(params.camera.viewToWorld, nearPos);
    float3 worldFar  = transformPoint(params.camera.viewToWorld, farPos);
    origin = worldNear;
    dir = make_float3(worldFar.x-worldNear.x, worldFar.y-worldNear.y, worldFar.z-worldNear.z);
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

static __forceinline__ __device__ float skySpectral(float3 dir, float lambda)
{
    dir = normalize3(dir);
    float t = fmaxf(0.f, fminf(1.f, dir.y*0.5f+0.5f));
    float scatter = 1.f;
    if (lambda > 400.f) { float ratio=460.f/lambda; scatter=ratio*ratio*ratio*ratio; }
    return 0.7f + (0.4f*scatter - 0.7f)*t;
}

static __forceinline__ __device__ float3 skyColor(float3 dir)
{
    dir = normalize3(dir);
    float t = fmaxf(0.f, fminf(1.f, dir.y*0.5f+0.5f));
    return make_float3(0.72f+(0.35f-0.72f)*t, 0.70f+(0.55f-0.70f)*t, 0.68f+(0.82f-0.68f)*t);
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
    double lm = lambda_nm * 1e-9;
    double h=6.62607015e-34, c=2.99792458e8, k=1.380649e-23;
    double x = (h*c)/(lm*k*temp);
    if (x > 500.0) return 0.f;
    double B = (2.0*h*c*c)/(lm*lm*lm*lm*lm) / (exp(x)-1.0);
    double peakL = 2.898e-3/temp;
    double xp = (h*c)/(peakL*k*temp);
    double Bp = (2.0*h*c*c)/(peakL*peakL*peakL*peakL*peakL) / (exp(xp)-1.0);
    return Bp > 0.0 ? float(B/Bp) : 0.f;
}

static __forceinline__ __device__ float lightEmission(const GPULight& light, float lambda)
{
    float spectrum;
    if (light.useColorTemp) {
        spectrum = blackbodyNorm(lambda, light.colorTemperature);
    } else {
        spectrum = light.color.x*spectralGauss(lambda,630.f,30.f)
                 + light.color.y*spectralGauss(lambda,532.f,30.f)
                 + light.color.z*spectralGauss(lambda,460.f,25.f);
    }
    return spectrum * light.intensity;
}

static __forceinline__ __device__ float3 lightDirection(const GPULight& light, float3 hitPos)
{
    if (light.type == 0 || light.type == 3) // distant or dome
        return normalize3(neg3(light.direction));
    float3 d = make_float3(light.position.x-hitPos.x, light.position.y-hitPos.y, light.position.z-hitPos.z);
    return normalize3(d);
}

static __forceinline__ __device__ float lightAttenuation(const GPULight& light, float3 hitPos)
{
    if (light.type == 0 || light.type == 3) return 1.f;
    float3 d = make_float3(light.position.x-hitPos.x, light.position.y-hitPos.y, light.position.z-hitPos.z);
    float dist2 = d.x*d.x + d.y*d.y + d.z*d.z;
    return 1.f / fmaxf(dist2, 0.001f);
}

// ---------------------------------------------------------------------------
// Dispersion + thin-film
// ---------------------------------------------------------------------------
static __forceinline__ __device__ float dispersedIOR(float ior_d, float abbe, float lambda)
{
    if (abbe <= 0.f) return ior_d;
    float disp = (ior_d - 1.f) / abbe;
    float dL = (587.6f - lambda) / (656.3f - 486.1f);
    return ior_d + disp * dL;
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
    const GPULight& light, float3 hitPos, float u1, float u2)
{
    if (light.type == 0 || light.type == 3)  // distant or dome
        return normalize3(neg3(light.direction));

    float3 samplePos = light.position;

    if (light.type == 1 && light.radius > 0.f) {
        // Sphere: uniform point on sphere surface
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
            float3 L = sampleLightDir(light, hitPos, hashRNG(rngSeed++), hashRNG(rngSeed++));

            // Shadow ray
            bool inShadow = false;
            float3 sOrig = make_float3(hitPos.x+N.x*0.01f, hitPos.y+N.y*0.01f, hitPos.z+N.z*0.01f);
            unsigned int sp0=0,sp1=0,sp2=0,sp3=__float_as_uint(1e30f),sp4=0,sp5=0,sp6=0;
            optixTrace(params.traversable, sOrig, L,
                       1e-4f, 1e30f, 0.f, OptixVisibilityMask(0xFF),
                       OPTIX_RAY_FLAG_CULL_BACK_FACING_TRIANGLES | OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT,
                       0, 1, 0, sp0,sp1,sp2,sp3,sp4,sp5,sp6);

            if (sp4 > 0u) {
                if (light.type == 0 || light.type == 3) {
                    inShadow = true;
                } else {
                    float3 toLight = make_float3(light.position.x-hitPos.x,
                        light.position.y-hitPos.y, light.position.z-hitPos.z);
                    if (__uint_as_float(sp3) < len3(toLight)) inShadow = true;
                }
            }

            if (!inShadow) {
                float bsdf = evalBSDF(mat, N, V, L, lambda);
                radiance += bsdf * lightEmission(light, lambda) * lightAttenuation(light, hitPos);
            }
        }
    }

    return radiance;
}

// ---------------------------------------------------------------------------
// __raygen__spectral
// ---------------------------------------------------------------------------
extern "C" __global__ void __raygen__spectral()
{
    const uint3 idx = optixGetLaunchIndex();
    const uint3 dim = optixGetLaunchDimensions();
    const unsigned int px=idx.x, py=idx.y;
    const unsigned int pixIdx = py*dim.x+px;
    const int spp = params.spp;
    const float W=float(dim.x), H=float(dim.y);

    if (spp <= 1) {
        // Normal-as-colour
        float3 origin, dir;
        makeRay(px+0.5f, py+0.5f, W, H, origin, dir);
        unsigned int p0=0,p1=0,p2=0,p3=__float_as_uint(1e30f),p4=0,p5=0,p6=0;
        optixTrace(params.traversable, origin, dir, 1e-4f,1e30f,0.f,
                   OptixVisibilityMask(0xFF), OPTIX_RAY_FLAG_NONE, 0,1,0, p0,p1,p2,p3,p4,p5,p6);
        params.framebuffer[pixIdx] = make_float4(
            __uint_as_float(p0),__uint_as_float(p1),__uint_as_float(p2),1.f);
        if (params.depthbuffer) params.depthbuffer[pixIdx] = __uint_as_float(p3);
    } else {
        // Spectral with BSDF + lights + bounces
        float X=0.f, Y=0.f, Z=0.f;
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
            makeRay(px+jx, py+jy, W, H, origin, dir);

            // Primary ray
            unsigned int p0=0,p1=0,p2=0,p3=__float_as_uint(1e30f),p4=0,p5=0,p6=0;
            optixTrace(params.traversable, origin, dir, 1e-4f,1e30f,0.f,
                       OptixVisibilityMask(0xFF), OPTIX_RAY_FLAG_NONE, 0,1,0, p0,p1,p2,p3,p4,p5,p6);

            float depth = __uint_as_float(p3);
            bool isHit = (p4 > 0u);

            float radiance = 0.f;

            if (isHit) {
                int matId = int(p4)-1;
                if (matId<0||matId>=int(params.materialCount)) matId=0;
                GPUMaterial mat = params.materials[matId];

                // Resolve texture at hit UV
                if (mat.baseColorTexId >= 0) {
                    float2 hitUV = make_float2(__uint_as_float(p5), __uint_as_float(p6));
                    mat.baseColor = sampleTextureGPU(mat.baseColorTexId, hitUV);
                }

                float3 N = normalize3(make_float3(
                    __uint_as_float(p0),__uint_as_float(p1),__uint_as_float(p2)));
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
                GPUMaterial resolvedMat = mat;  // mutable copy for texture resolution
                unsigned int bSeed = seed + 100u;
                bool isEntering = true;

                for (int bounce = 0; bounce < maxBounces; ++bounce) {
                    if (bounce >= 1) {
                        float rrProb = fminf(0.95f, throughput);
                        if (hashRNG(bSeed++) > rrProb) break;
                        throughput /= rrProb;
                    }

                    float bounceThroughput;
                    bool bTransmitted = false;
                    float3 bounceDir = sampleBounce(*bMat, bN, bV, lambda,
                        hashRNG(bSeed++), hashRNG(bSeed++), bounceThroughput, bTransmitted, isEntering);
                    if (bounceThroughput <= 0.f) break;
                    throughput *= bounceThroughput;

                    // Offset: along -N for transmission, +N for reflection
                    float bOff = bTransmitted ? -0.1f : 0.01f;
                    float3 bOrig = make_float3(bOrigin.x+bN.x*bOff, bOrigin.y+bN.y*bOff, bOrigin.z+bN.z*bOff);
                    unsigned int bp0=0,bp1=0,bp2=0,bp3=__float_as_uint(1e30f),bp4=0,bp5=0,bp6=0;
                    optixTrace(params.traversable, bOrig, bounceDir, 1e-4f,1e30f,0.f,
                               OptixVisibilityMask(0xFF), OPTIX_RAY_FLAG_NONE, 0,1,0, bp0,bp1,bp2,bp3,bp4,bp5,bp6);

                    if (bp4 == 0u) break;  // miss — no sky

                    float bDepth = __uint_as_float(bp3);
                    int bMatId = int(bp4)-1;
                    if (bMatId<0||bMatId>=int(params.materialCount)) bMatId=0;
                    resolvedMat = params.materials[bMatId];  // copy for texture resolution
                    if (resolvedMat.baseColorTexId >= 0) {
                        float2 bUV = make_float2(__uint_as_float(bp5), __uint_as_float(bp6));
                        resolvedMat.baseColor = sampleTextureGPU(resolvedMat.baseColorTexId, bUV);
                    }
                    bMat = &resolvedMat;

                    float3 rawN = normalize3(make_float3(
                        __uint_as_float(bp0),__uint_as_float(bp1),__uint_as_float(bp2)));
                    bV = normalize3(neg3(bounceDir));
                    float bNdV = dot3raw(rawN, bV);
                    isEntering = (bNdV > 0.f);
                    bN = (bNdV < 0.f) ? neg3(rawN) : rawN;
                    bOrigin = make_float3(bOrig.x+bounceDir.x*bDepth, bOrig.y+bounceDir.y*bDepth, bOrig.z+bounceDir.z*bDepth);

                    // Skip direct lighting inside transparent objects
                    bool insideGlass = (!isEntering && bMat->opacity < 0.99f);
                    if (!insideGlass)
                        radiance += throughput * shadeHit(*bMat, bN, bV, bOrigin, lambda, bSeed);
                }
            } else {
                radiance = 0.f;
            }

            float cx,cy,cz; cieXYZ(lambda,cx,cy,cz);
            float scale = 400.f/106.856895f;
            X+=radiance*cx*scale; Y+=radiance*cy*scale; Z+=radiance*cz*scale;
        }

        float inv=1.f/float(spp); X*=inv; Y*=inv; Z*=inv;
        float r= 3.2406f*X-1.5372f*Y-0.4986f*Z;
        float g=-0.9689f*X+1.8758f*Y+0.0415f*Z;
        float b= 0.0557f*X-0.2040f*Y+1.0570f*Z;
        params.framebuffer[pixIdx] = make_float4(fmaxf(0.f,r),fmaxf(0.f,g),fmaxf(0.f,b),1.f);
        if (params.depthbuffer) params.depthbuffer[pixIdx] = minDepth;
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
