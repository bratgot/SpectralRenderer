#include "SpectralIntegrator.h"

#ifdef SPECTRAL_HAS_EMBREE
#include "SpectralBVH.h"
#endif

#include <pxr/base/gf/vec4d.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>
#include <execution>

PXR_NAMESPACE_OPEN_SCOPE

// Forward declaration — defined later in this file
static SpectralMaterial _ResolveMaterial(
    const SpectralMaterial& mat, const SpectralTriangle& tri,
    float w, float u, float v, const SpectralScene& scene);

// ---------------------------------------------------------------------------
// Procedural fBm noise — deterministic, no tables, world-space
// Ported from VDBmarcher (Marten Blumen)
// ---------------------------------------------------------------------------
static double _noiseHash3(double x, double y, double z) {
    int ix = (int)std::floor(x), iy = (int)std::floor(y), iz = (int)std::floor(z);
    double fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);
    fz = fz * fz * (3.0 - 2.0 * fz);
    auto h = [](int a, int b, int c) -> double {
        uint32_t u = (uint32_t)a * 1664525u ^ (uint32_t)b * 1013904223u ^ (uint32_t)c * 2246822519u;
        u ^= u >> 16; u *= 0x45d9f3bu; u ^= u >> 16;
        return (int)(u & 0xFFFFu) / 32768.0 - 1.0;
    };
    double v000=h(ix,iy,iz),     v100=h(ix+1,iy,iz);
    double v010=h(ix,iy+1,iz),   v110=h(ix+1,iy+1,iz);
    double v001=h(ix,iy,iz+1),   v101=h(ix+1,iy,iz+1);
    double v011=h(ix,iy+1,iz+1), v111=h(ix+1,iy+1,iz+1);
    return v000 + fx*(v100-v000)
         + fy*(v010-v000 + fx*(v110-v010-v100+v000))
         + fz*(v001-v000 + fx*(v101-v001-v100+v000)
               + fy*(v011-v001-v010+v000
                    + fx*(v111-v011-v101+v001-v110+v010+v100-v000)));
}

static double _noiseFBm(double x, double y, double z, int octaves, double roughness) {
    double val = 0.0, amp = 1.0, total = 0.0, freq = 1.0;
    for (int i = 0; i < octaves; ++i) {
        val += amp * _noiseHash3(x * freq, y * freq, z * freq);
        total += amp; amp *= roughness; freq *= 2.0;
    }
    return (total > 0.0) ? val / total : 0.0;
}

// ---------------------------------------------------------------------------
// RenderFrame  — full image, parallel over rows
// ---------------------------------------------------------------------------
void SpectralIntegrator::RenderFrame(
    const SpectralScene&  scene,
    const SpectralCamera& camera,
    float*                pixels,
    int                   spp,
    float*                depthOut,
    int                   maxBounces,
    float*                objectIdOut,
    float*                materialIdOut,
    const AOVBuffers*     aovs,
    float*                aoOut,
    const SpectralPhotonMap* photonMap,
    float                 gatherRadius,
    int                   colorSpace,
    const SpectralVolume* const* volumes,
    int                   numVolumes)
{
#ifdef SPECTRAL_HAS_EMBREE
    SpectralBVH bvh;
    bvh.Build(scene);

    // Volume debug
    if (numVolumes > 0 && volumes)
        fprintf(stderr, "SpectralRender: %d volume(s) active\n", numVolumes);
    else
        fprintf(stderr, "SpectralRender: no volume\n");

    const unsigned int W = camera.imageWidth;
    const unsigned int H = camera.imageHeight;
    const bool spectral = (spp > 1);

    // World-to-camera matrix for converting hit points to camera-space Z.
    // Nuke deep expects Z depth along the camera's view axis, not ray distance.
    const GfMatrix4d worldToView = camera.viewToWorld.GetInverse();

    std::vector<unsigned int> rows(H);
    std::iota(rows.begin(), rows.end(), 0u);

    std::for_each(
        std::execution::par_unseq,
        rows.begin(), rows.end(),
        [&](unsigned int imageY)
        {
            for (unsigned int imageX = 0; imageX < W; ++imageX) {
                const size_t pixIdx = imageY * W + imageX;

                if (!spectral) {
                    // ---- SPP=1: fast normal-as-colour mode ----
                    GfRay ray = _MakeRay(camera, imageX, imageY);
                    SpectralBVH::Hit hit = bvh.Intersect(ray);

                    GfVec3f color;
                    float depth = 1e30f;
                    int hitObjectId = 0, hitMaterialId = 0;
                    if (hit.valid()) {
                        color = _ShadeSmoothNormal(
                            *hit.tri,
                            static_cast<double>(hit.u),
                            static_cast<double>(hit.v));
                        GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                        GfVec3d viewHit = worldToView.Transform(worldHit);
                        depth = static_cast<float>(-viewHit[2]);
                        hitObjectId = hit.tri->objectId;
                        hitMaterialId = hit.tri->materialId;
                    } else {
                        color = GfVec3f(0.f);
                    }

                    float* px = pixels + pixIdx * 4;
                    px[0] = color[0];
                    px[1] = color[1];
                    px[2] = color[2];
                    px[3] = 1.0f;

                    if (depthOut) depthOut[pixIdx] = depth;
                    if (objectIdOut) objectIdOut[pixIdx] = static_cast<float>(hitObjectId);
                    if (materialIdOut) materialIdOut[pixIdx] = static_cast<float>(hitMaterialId);

                } else {
                    // ---- SPP>1: hero wavelength spectral mode ----
                    // Handled below in adaptive sampling pass
                }
            }
        });

    // ---- Adaptive spectral rendering ----
    if (spectral) {
        const size_t numPixels = size_t(W) * H;

        // Per-pixel accumulators
        std::vector<float> accX(numPixels, 0.f);
        std::vector<float> accY(numPixels, 0.f);
        std::vector<float> accZ(numPixels, 0.f);
        std::vector<float> accLumSum(numPixels, 0.f);   // for variance
        std::vector<float> accLumSqSum(numPixels, 0.f); // for variance
        std::vector<int>   accCount(numPixels, 0);
        std::vector<float> accAlpha(numPixels, 0.f);
        std::vector<bool>  converged(numPixels, false);
        std::vector<float> depthBuf(numPixels, 1e30f);
        std::vector<int>   objIdBuf(numPixels, 0);
        std::vector<int>   matIdBuf(numPixels, 0);

        // Shading component accumulators (XYZ per component)
        bool trackDirect   = aovs && aovs->direct;
        bool trackIndirect = aovs && aovs->indirect;
        bool trackEmission = aovs && aovs->emission;
        std::vector<float> accDirectX, accDirectY, accDirectZ;
        std::vector<float> accIndirectX, accIndirectY, accIndirectZ;
        std::vector<float> accEmissionX, accEmissionY, accEmissionZ;
        if (trackDirect)   { accDirectX.resize(numPixels,0); accDirectY.resize(numPixels,0); accDirectZ.resize(numPixels,0); }
        if (trackIndirect) { accIndirectX.resize(numPixels,0); accIndirectY.resize(numPixels,0); accIndirectZ.resize(numPixels,0); }
        if (trackEmission) { accEmissionX.resize(numPixels,0); accEmissionY.resize(numPixels,0); accEmissionZ.resize(numPixels,0); }

        // LPE accumulators
        bool trackLPE = aovs && (aovs->diffuseDirect || aovs->specularDirect ||
                                  aovs->diffuseIndirect || aovs->specularIndirect ||
                                  aovs->transmission);
        std::vector<float> accDiffDirX, accDiffDirY, accDiffDirZ;
        std::vector<float> accSpecDirX, accSpecDirY, accSpecDirZ;
        std::vector<float> accDiffIndX, accDiffIndY, accDiffIndZ;
        std::vector<float> accSpecIndX, accSpecIndY, accSpecIndZ;
        std::vector<float> accTransX, accTransY, accTransZ;
        if (trackLPE) {
            accDiffDirX.resize(numPixels,0); accDiffDirY.resize(numPixels,0); accDiffDirZ.resize(numPixels,0);
            accSpecDirX.resize(numPixels,0); accSpecDirY.resize(numPixels,0); accSpecDirZ.resize(numPixels,0);
            accDiffIndX.resize(numPixels,0); accDiffIndY.resize(numPixels,0); accDiffIndZ.resize(numPixels,0);
            accSpecIndX.resize(numPixels,0); accSpecIndY.resize(numPixels,0); accSpecIndZ.resize(numPixels,0);
            accTransX.resize(numPixels,0); accTransY.resize(numPixels,0); accTransZ.resize(numPixels,0);
        }

        // Split into adaptive passes
        const int batchSize = std::max(4, spp / 4);
        const float adaptThreshold = camera.adaptiveThreshold;
        int totalSampled = 0;

        for (int passStart = 0; passStart < spp; passStart += batchSize) {
            const int passEnd = std::min(passStart + batchSize, spp);
            const int passSpp = passEnd - passStart;

            std::for_each(
                std::execution::par_unseq,
                rows.begin(), rows.end(),
                [&](unsigned int imageY)
                {
                    for (unsigned int imageX = 0; imageX < W; ++imageX) {
                        const size_t pixIdx = imageY * W + imageX;

                        // Skip converged pixels (after first pass)
                        if (converged[pixIdx]) continue;

                        for (int s = passStart; s < passEnd; ++s) {
                            unsigned int seed = (imageY * W + imageX) * 1031 + s * 6571;

                            // R2 quasi-random sequence for pixel jitter + wavelength
                            // Gives blue-noise-like distribution without a texture
                            // α₁ = 1/φ₂ ≈ 0.7548776662, α₂ = 1/φ₂² ≈ 0.5698402909
                            // Cranley-Patterson rotation per pixel for decorrelation
                            unsigned int pixSeed = imageY * W + imageX;
                            float r2_offset1 = _Hash(pixSeed * 2u);
                            float r2_offset2 = _Hash(pixSeed * 2u + 1u);

                            float jx, jy, wu;
                            if (camera.blueNoise) {
                                jx = std::fmod(r2_offset1 + float(s) * 0.7548776662f, 1.f);
                                jy = std::fmod(r2_offset2 + float(s) * 0.5698402909f, 1.f);
                                wu = std::fmod(_Hash(pixSeed * 3u) + float(s) * 0.7548776662f, 1.f);
                            } else {
                                jx = _Hash(seed);
                                jy = _Hash(seed + 1);
                                wu = (float(s) + _Hash(seed + 2)) / float(spp);
                            }
                            float lambda = SpectralSpectrum::SampleWavelength(wu);

                            float rayTime = camera.shutterOpen +
                                _Hash(seed + 3) * (camera.shutterClose - camera.shutterOpen);

                            GfRay ray = _MakeRay(camera, imageX, imageY, jx, jy);
                            SpectralBVH::Hit hit = bvh.Intersect(ray, rayTime);

                            float radiance;
                            ShadeComponents comps;
                            if (hit.valid()) {
                                const SpectralMaterial& mat = scene.GetMaterial(hit.tri->materialId);
                                GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                                GfVec3f hitPos = GfVec3f(worldHit);
                                GfVec3f rayDir = GfVec3f(ray.GetDirection());
                                unsigned int bounceSeed = seed + 100u;
                                int shadeBounces = std::max(maxBounces, camera.refractionBounces);
                                radiance = _ShadeSpectral(
                                    *hit.tri,
                                    static_cast<double>(hit.u),
                                    static_cast<double>(hit.v),
                                    lambda, mat, scene, hitPos, rayDir,
                                    shadeBounces, bounceSeed, bvh, rayTime, &comps,
                                    photonMap, gatherRadius);

                                GfVec3d viewHit = worldToView.Transform(worldHit);
                                float camZ = static_cast<float>(-viewHit[2]);
                                if (camZ < depthBuf[pixIdx]) depthBuf[pixIdx] = camZ;
                                if (objIdBuf[pixIdx] == 0) {
                                    objIdBuf[pixIdx] = hit.tri->objectId;
                                    matIdBuf[pixIdx] = hit.tri->materialId;

                                    // Geometry AOVs — first hit only
                                    if (aovs) {
                                        float w = 1.f - float(hit.u) - float(hit.v);
                                        GfVec3f N = hit.tri->n0 * w + hit.tri->n1 * float(hit.u) + hit.tri->n2 * float(hit.v);
                                        float nlen = N.GetLength();
                                        if (nlen > 1e-6f) N /= nlen;
                                        GfVec3f V = GfVec3f(-rayDir);
                                        float vlen = V.GetLength();
                                        if (vlen > 1e-6f) V /= vlen;
                                        if (N[0]*V[0]+N[1]*V[1]+N[2]*V[2] < 0.f) N = -N;

                                        if (aovs->normal) {
                                            aovs->normal[pixIdx*3+0] = N[0];
                                            aovs->normal[pixIdx*3+1] = N[1];
                                            aovs->normal[pixIdx*3+2] = N[2];
                                        }
                                        if (aovs->position) {
                                            aovs->position[pixIdx*3+0] = hitPos[0];
                                            aovs->position[pixIdx*3+1] = hitPos[1];
                                            aovs->position[pixIdx*3+2] = hitPos[2];
                                        }
                                        if (aovs->pRef) {
                                            GfVec3f pRef = hit.tri->pRef0 * w + hit.tri->pRef1 * float(hit.u) + hit.tri->pRef2 * float(hit.v);
                                            aovs->pRef[pixIdx*3+0] = pRef[0];
                                            aovs->pRef[pixIdx*3+1] = pRef[1];
                                            aovs->pRef[pixIdx*3+2] = pRef[2];
                                        }
                                        if (aovs->uv) {
                                            GfVec2f uv = hit.tri->uv0 * w + hit.tri->uv1 * float(hit.u) + hit.tri->uv2 * float(hit.v);
                                            aovs->uv[pixIdx*2+0] = uv[0];
                                            aovs->uv[pixIdx*2+1] = uv[1];
                                        }
                                        if (aovs->albedo) {
                                            SpectralMaterial resolved = _ResolveMaterial(mat, *hit.tri, w, float(hit.u), float(hit.v), scene);
                                            aovs->albedo[pixIdx*3+0] = resolved.baseColor[0];
                                            aovs->albedo[pixIdx*3+1] = resolved.baseColor[1];
                                            aovs->albedo[pixIdx*3+2] = resolved.baseColor[2];
                                        }
                                    }
                                }
                            } else {
                                // Primary ray miss
                                radiance = 0.f;
                                // Show dome light environment on miss only when no volume
                                // (volume scenes: background should be transparent for comp)
                                bool hasAnyVolume = false;
                                for (int vv = 0; vv < numVolumes; ++vv) {
                                    if (volumes[vv] && volumes[vv]->IsValid()) { hasAnyVolume = true; break; }
                                }
                                if (!hasAnyVolume) {
                                    GfVec3f missDir = GfVec3f(ray.GetDirection());
                                    float mLen = missDir.GetLength();
                                    if (mLen > 1e-6f) missDir /= mLen;
                                    for (const SpectralLight& light : scene.GetLights()) {
                                        if (light.type == SpectralLight::Type::Dome) {
                                            radiance += light.EnvironmentEmission(missDir, lambda);
                                        }
                                    }
                                }
                            }

                            // Volume ray marching — DIRECT RGB (not spectral)
                            // Volumes render in RGB for proper color at any spp.
                            // Surface radiance still uses spectral wavelength sampling.
                            float finalVolTrans = 1.f;
                            GfVec3f totalVolRGB(0.f);
                            float vx = 0.f, vy = 0.f, vz = 0.f;
                            for (int vi = 0; vi < numVolumes; ++vi) {
                                const SpectralVolume* volume = volumes[vi];
                                if (!volume || !volume->IsValid()) continue;
                                GfVec3f volRGB(0.f);  // per-volume scatter — reset each volume
                                // Ray-AABB intersection
                                GfVec3f ro(ray.GetStartPoint());
                                GfVec3f rd(ray.GetDirection());
                                float rdLen = rd.GetLength();
                                if (rdLen > 1e-8f) rd /= rdLen;

                                GfVec3f invDir(1.f/(std::abs(rd[0])>1e-8f?rd[0]:1e-8f),
                                               1.f/(std::abs(rd[1])>1e-8f?rd[1]:1e-8f),
                                               1.f/(std::abs(rd[2])>1e-8f?rd[2]:1e-8f));
                                GfVec3f t0v = GfVec3f(
                                    (volume->GetBboxMin()[0]-ro[0])*invDir[0],
                                    (volume->GetBboxMin()[1]-ro[1])*invDir[1],
                                    (volume->GetBboxMin()[2]-ro[2])*invDir[2]);
                                GfVec3f t1v = GfVec3f(
                                    (volume->GetBboxMax()[0]-ro[0])*invDir[0],
                                    (volume->GetBboxMax()[1]-ro[1])*invDir[1],
                                    (volume->GetBboxMax()[2]-ro[2])*invDir[2]);
                                float tNear = std::max({std::min(t0v[0],t1v[0]),
                                                        std::min(t0v[1],t1v[1]),
                                                        std::min(t0v[2],t1v[2])});
                                float tFar  = std::min({std::max(t0v[0],t1v[0]),
                                                        std::max(t0v[1],t1v[1]),
                                                        std::max(t0v[2],t1v[2])});
                                tNear = std::max(tNear, 0.f);

                                // Surface hit limits the far distance
                                float surfaceT = hit.valid() ? float(hit.t) : 1e30f;
                                tFar = std::min(tFar, surfaceT);

                                if (tNear < tFar) {
                                    // Step size from quality parameter
                                    GfVec3f bboxSize = volume->GetBboxMax() - volume->GetBboxMin();
                                    float voxelSize = std::max({bboxSize[0]/volume->resX,
                                                                bboxSize[1]/volume->resY,
                                                                bboxSize[2]/volume->resZ});
                                    float q = std::max(1.f, volume->quality);
                                    float dt = (volume->stepSize > 0.01f) ? volume->stepSize
                                               : voxelSize / (q * q * 0.25f);
                                    int maxSteps = std::min(1024, int((tFar - tNear) / dt) + 1);

                                    // Jitter first step
                                    float jitterOff = volume->jitter ? _Hash(seed + 77) * dt : 0.f;
                                    float t = tNear + jitterOff;

                                    float volTransmittance = 1.f;

                                    // Collect all non-dome lights for directional scatter
                                    struct VolLight { GfVec3f dir; GfVec3f color; GfVec3f pos; int type; };
                                    std::vector<VolLight> volLights;
                                    if (scene.HasLights()) {
                                        for (const auto& L : scene.GetLights()) {
                                            if (L.type == SpectralLight::Type::Dome) continue;
                                            float li = L.color.GetLength() * L.intensity;
                                            if (li < 0.001f) continue;
                                            volLights.push_back({L.direction, L.color * L.intensity, L.position, int(L.type)});
                                        }
                                    }

                                    for (int step = 0; step < maxSteps && t < tFar; ++step, t += dt) {
                                        GfVec3f p = ro + rd * t;
                                        GfVec3f uv = volume->WorldToNorm(p);
                                        float density = volume->SampleDensity(uv[0], uv[1], uv[2]);

                                        // Procedural fBm noise — perturbs density at render time
                                        if (volume->noiseEnable && density > 1e-6f) {
                                            float bboxDiag = (volume->GetBboxMax() - volume->GetBboxMin()).GetLength();
                                            float ns = volume->noiseNormalize
                                                ? volume->noiseScale / std::max(bboxDiag, 1e-4f)
                                                : volume->noiseScale;
                                            double n = _noiseFBm(p[0]*ns, p[1]*ns, p[2]*ns,
                                                                 volume->noiseOctaves, volume->noiseRoughness);
                                            density = std::max(0.f, density * float(1.0 + volume->noiseStrength * n));
                                        }

                                        // Adaptive stepping: skip faster in thin regions
                                        if (density < 1e-5f) {
                                            if (volume->adaptiveStep) t += dt * 3.f; // 4x step in empty
                                            continue;
                                        }

                                        // Beer-Lambert extinction
                                        float sigma_t = density * volume->extinction;

                                        // Chromatic extinction
                                        if (volume->chromaticExtinction) {
                                            float wt;
                                            if (lambda < 500.f) wt = volume->sigmaB;
                                            else if (lambda < 580.f) wt = volume->sigmaG;
                                            else wt = volume->sigmaR;
                                            sigma_t *= wt;
                                        }

                                        float stepTrans = std::exp(-sigma_t * dt);
                                        float absorption = 1.f - stepTrans;

                                        GfVec3f stepRGB(0.f);
                                        int rmode = volume->renderMode;

                                        if (rmode == 1) { // Greyscale
                                            float v = density * volume->intensity;
                                            stepRGB = GfVec3f(v);
                                        } else if (rmode == 2) { // Heat ramp
                                            float t = std::min(density * volume->extinction * 0.5f, 1.f) * volume->intensity;
                                            stepRGB = GfVec3f(std::min(t*3.f,1.f), std::max(0.f,(t-.33f)*3.f), std::max(0.f,(t-.66f)*3.f));
                                        } else if (rmode == 3) { // Cool ramp
                                            float t = std::min(density * volume->extinction * 0.5f, 1.f) * volume->intensity;
                                            stepRGB = GfVec3f(std::max(0.f,(t-.5f)*2.f), std::max(0.f,(t-.25f)*2.f), std::min(t*2.f,1.f));
                                        } else if (rmode == 4) { // Blackbody RGB
                                            float temp = volume->SampleTemperature(uv[0], uv[1], uv[2]);
                                            if (temp > volume->tempMin) {
                                                float tN = std::min((temp-volume->tempMin)/(volume->tempMax-volume->tempMin+1e-6f), 1.f);
                                                float T = volume->tempMin + tN*(volume->tempMax-volume->tempMin);
                                                float r=1,g=1,b=1, t100=T/100.f;
                                                if (T<=6600.f) { r=1; g=std::max(0.f,std::min(0.39f*std::log(t100)-0.63f,1.f)); b=std::max(0.f,std::min(0.54f*std::log(t100-10.f)-1.19f,1.f)); }
                                                else { r=std::max(0.f,std::min(1.29f*std::pow(t100-60.f,-0.13f),1.f)); g=std::max(0.f,std::min(1.13f*std::pow(t100-60.f,-0.07f),1.f)); b=1; }
                                                stepRGB = GfVec3f(r,g,b) * volume->emissionIntensity * density * volume->intensity;
                                            }
                                        } else { // Lit (0) / Explosion (5) — full RGB lighting
                                            float powder = 1.f;
                                            if (volume->powderStrength > 0.01f) powder = 1.f - std::exp(-density * volume->powderStrength * 2.f);

                                            GfVec3f scatRGB(0.f);
                                            for (const auto& vl : volLights) {
                                                GfVec3f lDir = vl.dir;
                                                if (vl.type==int(SpectralLight::Type::Sphere)||vl.type==int(SpectralLight::Type::Spot)) {
                                                    lDir = p - vl.pos; float len = lDir.GetLength(); if (len>1e-6f) lDir /= len; }
                                                float cosTheta = -(rd[0]*lDir[0]+rd[1]*lDir[1]+rd[2]*lDir[2]);

                                                // Phase function — mode 0: dual-lobe HG, mode 1: Cornette-Shanks Mie
                                                float phase;
                                                if (volume->phaseMode == 1) {
                                                    // Cornette-Shanks approximation to Mie scattering
                                                    // g derived from droplet diameter (Jendersie & d'Eon 2023 fit)
                                                    float d = volume->mieDropletD;
                                                    float g = 0.85f * (1.f - std::exp(-d * 0.8f)); // saturates at ~0.85 for large drops
                                                    float g2 = g*g;
                                                    float num = (1.f-g2) * (1.f+cosTheta*cosTheta);
                                                    float denom = (2.f+g2) * std::pow(std::max(1.f+g2-2.f*g*cosTheta, 1e-4f), 1.5f);
                                                    phase = (3.f/(8.f*3.14159f)) * num / denom;
                                                } else {
                                                    float gF=volume->gForward, gB=volume->gBackward;
                                                    float denomF=1.f+gF*gF-2.f*gF*cosTheta, denomB=1.f+gB*gB-2.f*gB*cosTheta;
                                                    float phaseF=(1.f-gF*gF)/(4.f*3.14159f*std::pow(std::max(denomF,1e-4f),1.5f));
                                                    float phaseB=(1.f-gB*gB)/(4.f*3.14159f*std::pow(std::max(denomB,1e-4f),1.5f));
                                                    phase = volume->lobeMix*phaseF + (1.f-volume->lobeMix)*phaseB;
                                                }
                                                float shadowTrans = 1.f;
                                                if (volume->shadowSteps>0 && volume->shadowDensity>0.01f) {
                                                    GfVec3f sDir = -lDir;
                                                    float sDt = bboxSize.GetLength()/float(volume->shadowSteps);
                                                    for (int ss=1; ss<=volume->shadowSteps; ++ss) {
                                                        GfVec3f sp = p+sDir*(sDt*ss); GfVec3f suv = volume->WorldToNorm(sp);
                                                        if (suv[0]<0||suv[0]>1||suv[1]<0||suv[1]>1||suv[2]<0||suv[2]>1) break;
                                                        shadowTrans *= std::exp(-volume->SampleDensity(suv[0],suv[1],suv[2])*volume->extinction*volume->shadowDensity*sDt);
                                                        if (shadowTrans<0.001f) break; } }
                                                GfVec3f lRGB(vl.color[0]*volume->scatterColor[0], vl.color[1]*volume->scatterColor[1], vl.color[2]*volume->scatterColor[2]);
                                                scatRGB += lRGB * (volume->scattering*density*phase*shadowTrans*powder);
                                            }
                                            // Dome ambient RGB (uses HDRI average if available)
                                            // Dome illuminates from ALL directions: phase integral over sphere = 1
                                            if (scene.HasLights()) for (const auto& L : scene.GetLights()) {
                                                if (L.type!=SpectralLight::Type::Dome) continue;
                                                GfVec3f domeCol = (L.envPixels && L.envWidth > 0)
                                                    ? L.envAvgColor * L.intensity * volume->envIntensity
                                                    : L.color * L.intensity * volume->envIntensity;
                                                scatRGB += domeCol*volume->envDiffuse * (volume->scattering*density); }
                                            // Analytical MS RGB (Wrenninge 2015)
                                            if (volume->msApprox && volume->scattering>0.01f) {
                                                float albedo = volume->scattering/std::max(volume->extinction,0.01f);
                                                float msB = albedo/std::max(1.f-albedo*volume->gForward*volume->lobeMix,0.01f);
                                                scatRGB += volume->msTint*(density*volume->scattering*msB*0.3f); }
                                            stepRGB = scatRGB * volume->intensity;
                                        }

                                        // Scatter through absorption
                                        volRGB += stepRGB * (volTransmittance * absorption);

                                        // Emission RGB — bypasses absorption
                                        if (rmode==0||rmode==4||rmode==5||volume->emissionIntensity>0.01f) {
                                            GfVec3f emRGB(0.f);
                                            float temp = volume->SampleTemperature(uv[0],uv[1],uv[2]);
                                            if (temp > volume->tempMin) {
                                                float tN = std::min((temp-volume->tempMin)/(volume->tempMax-volume->tempMin+1e-6f),1.f);
                                                float T = volume->tempMin+tN*(volume->tempMax-volume->tempMin);
                                                float r=1,g=1,b=1, t100=T/100.f;
                                                if (T<=6600.f) { r=1; g=std::max(0.f,std::min(0.39f*std::log(t100)-0.63f,1.f)); b=std::max(0.f,std::min(0.54f*std::log(t100-10.f)-1.19f,1.f)); }
                                                else { r=std::max(0.f,std::min(1.29f*std::pow(t100-60.f,-0.13f),1.f)); g=std::max(0.f,std::min(1.13f*std::pow(t100-60.f,-0.07f),1.f)); b=1; }
                                                emRGB += GfVec3f(r,g,b)*(volume->emissionIntensity*tN); }
                                            float fl = volume->SampleFlame(uv[0],uv[1],uv[2]);
                                            if (fl>0.01f) {
                                                float T2=1500.f+fl*2000.f, t100=T2/100.f;
                                                float r=1,g=std::max(0.f,std::min(0.39f*std::log(t100)-0.63f,1.f)),b=std::max(0.f,std::min(0.54f*std::log(t100-10.f)-1.19f,1.f));
                                                emRGB += GfVec3f(r,g,b)*(volume->flameIntensity*fl); }
                                            volRGB += emRGB*(volTransmittance*density*dt*volume->intensity);
                                        }
                                        volTransmittance *= stepTrans;

                                        if (volTransmittance < 0.001f) break;
                                    }

                                    // Composite volume over surface
                                    if (volume->spectralVolumes) {
                                        int sc = (lambda < 500.f) ? 2 : (lambda < 580.f) ? 1 : 0;
                                        float volSpec = volRGB[sc];
                                        radiance = volSpec + volTransmittance * radiance;
                                    } else {
                                        // Accumulate this volume's XYZ contribution
                                        vx += (0.4124f*volRGB[0] + 0.3576f*volRGB[1] + 0.1805f*volRGB[2]) * finalVolTrans;
                                        vy += (0.2126f*volRGB[0] + 0.7152f*volRGB[1] + 0.0722f*volRGB[2]) * finalVolTrans;
                                        vz += (0.0193f*volRGB[0] + 0.1192f*volRGB[1] + 0.9505f*volRGB[2]) * finalVolTrans;
                                        radiance *= volTransmittance;
                                    }
                                    finalVolTrans *= volTransmittance;  // multiply, don't overwrite
                                }
                            }

                            // Per-sample alpha
                            {
                                float sampleAlpha = hit.valid() ? 1.f : 0.f;
                                sampleAlpha = 1.f - finalVolTrans * (1.f - sampleAlpha);
                                accAlpha[pixIdx] += sampleAlpha;
                            }

                            // Firefly clamp: limit extreme spectral outliers
                            // Rough glass + dispersion can produce very bright
                            // single-wavelength pixels. Clamping at 100x average
                            // removes these without affecting normal renders.
                            if (radiance > 0.f && accCount[pixIdx] > 0) {
                                float avg = (accX[pixIdx] + accY[pixIdx] + accZ[pixIdx])
                                          / (3.f * accCount[pixIdx] + 1e-6f);
                                float maxVal = std::max(100.f, avg * 100.f);
                                radiance = std::min(radiance, maxVal);
                            }

                            GfVec3f xyz = SpectralSpectrum::RadianceToXYZ(radiance, lambda);
                            // Volume contributes RGB directly (bypasses spectral)
                            accX[pixIdx] += xyz[0] + vx;
                            accY[pixIdx] += xyz[1] + vy;
                            accZ[pixIdx] += xyz[2] + vz;
                            accCount[pixIdx]++;

                            // Accumulate shading components
                            if (trackDirect) {
                                GfVec3f d = SpectralSpectrum::RadianceToXYZ(comps.direct, lambda);
                                accDirectX[pixIdx] += d[0]; accDirectY[pixIdx] += d[1]; accDirectZ[pixIdx] += d[2];
                            }
                            if (trackIndirect) {
                                GfVec3f i = SpectralSpectrum::RadianceToXYZ(comps.indirect, lambda);
                                accIndirectX[pixIdx] += i[0]; accIndirectY[pixIdx] += i[1]; accIndirectZ[pixIdx] += i[2];
                            }
                            if (trackEmission) {
                                GfVec3f e = SpectralSpectrum::RadianceToXYZ(comps.emission, lambda);
                                accEmissionX[pixIdx] += e[0]; accEmissionY[pixIdx] += e[1]; accEmissionZ[pixIdx] += e[2];
                            }
                            if (trackLPE) {
                                GfVec3f dd = SpectralSpectrum::RadianceToXYZ(comps.diffuseDirect, lambda);
                                accDiffDirX[pixIdx] += dd[0]; accDiffDirY[pixIdx] += dd[1]; accDiffDirZ[pixIdx] += dd[2];
                                GfVec3f sd = SpectralSpectrum::RadianceToXYZ(comps.specularDirect, lambda);
                                accSpecDirX[pixIdx] += sd[0]; accSpecDirY[pixIdx] += sd[1]; accSpecDirZ[pixIdx] += sd[2];
                                GfVec3f di = SpectralSpectrum::RadianceToXYZ(comps.diffuseIndirect, lambda);
                                accDiffIndX[pixIdx] += di[0]; accDiffIndY[pixIdx] += di[1]; accDiffIndZ[pixIdx] += di[2];
                                GfVec3f si = SpectralSpectrum::RadianceToXYZ(comps.specularIndirect, lambda);
                                accSpecIndX[pixIdx] += si[0]; accSpecIndY[pixIdx] += si[1]; accSpecIndZ[pixIdx] += si[2];
                                GfVec3f tr = SpectralSpectrum::RadianceToXYZ(comps.transmission, lambda);
                                accTransX[pixIdx] += tr[0]; accTransY[pixIdx] += tr[1]; accTransZ[pixIdx] += tr[2];
                            }

                            // Track luminance variance (Y channel ≈ luminance)
                            float lum = xyz[1];
                            accLumSum[pixIdx] += lum;
                            accLumSqSum[pixIdx] += lum * lum;
                        }
                    }
                });

            totalSampled += passSpp;

            // After first pass, check convergence
            if (passStart > 0 && adaptThreshold > 0.f) {
                int convergedCount = 0;
                for (size_t i = 0; i < numPixels; ++i) {
                    if (converged[i]) { convergedCount++; continue; }
                    int n = accCount[i];
                    if (n < 8) continue;  // need minimum samples for stable variance
                    float mean = accLumSum[i] / float(n);
                    float var = accLumSqSum[i] / float(n) - mean * mean;
                    float stddev = std::sqrt(std::max(0.f, var));
                    if (stddev < adaptThreshold * std::max(mean, 0.001f)) {
                        converged[i] = true;
                        convergedCount++;
                    }
                }
                if (passStart + batchSize < spp) {
                    fprintf(stderr, "SpectralRender: adaptive pass %d/%d — %d/%zu pixels converged (%.0f%%)\n",
                            passStart / batchSize + 1, (spp + batchSize - 1) / batchSize,
                            convergedCount, numPixels,
                            100.f * convergedCount / numPixels);
                }
            }
        }

        // Write final pixel values
        for (size_t i = 0; i < numPixels; ++i) {
            int n = accCount[i];
            if (n == 0) n = 1;
            float invN = 1.f / float(n);
            GfVec3f rgb = SpectralSpectrum::XYZtoRGB(
                accX[i] * invN, accY[i] * invN, accZ[i] * invN,
                static_cast<SpectralSpectrum::ColorSpace>(colorSpace));
            float* px = pixels + i * 4;
            px[0] = std::max(0.f, rgb[0]);
            px[1] = std::max(0.f, rgb[1]);
            px[2] = std::max(0.f, rgb[2]);
            px[3] = (n > 0) ? std::min(1.f, accAlpha[i] / float(n)) : 0.f;

            if (depthOut) depthOut[i] = depthBuf[i];
            if (objectIdOut) objectIdOut[i] = static_cast<float>(objIdBuf[i]);
            if (materialIdOut) materialIdOut[i] = static_cast<float>(matIdBuf[i]);

            // Write shading component AOVs
            if (trackDirect) {
                GfVec3f c = SpectralSpectrum::XYZtoRGB(accDirectX[i]*invN, accDirectY[i]*invN, accDirectZ[i]*invN, static_cast<SpectralSpectrum::ColorSpace>(colorSpace));
                aovs->direct[i*3+0] = std::max(0.f,c[0]); aovs->direct[i*3+1] = std::max(0.f,c[1]); aovs->direct[i*3+2] = std::max(0.f,c[2]);
            }
            if (trackIndirect) {
                GfVec3f c = SpectralSpectrum::XYZtoRGB(accIndirectX[i]*invN, accIndirectY[i]*invN, accIndirectZ[i]*invN, static_cast<SpectralSpectrum::ColorSpace>(colorSpace));
                aovs->indirect[i*3+0] = std::max(0.f,c[0]); aovs->indirect[i*3+1] = std::max(0.f,c[1]); aovs->indirect[i*3+2] = std::max(0.f,c[2]);
            }
            if (trackEmission) {
                GfVec3f c = SpectralSpectrum::XYZtoRGB(accEmissionX[i]*invN, accEmissionY[i]*invN, accEmissionZ[i]*invN, static_cast<SpectralSpectrum::ColorSpace>(colorSpace));
                aovs->emission[i*3+0] = std::max(0.f,c[0]); aovs->emission[i*3+1] = std::max(0.f,c[1]); aovs->emission[i*3+2] = std::max(0.f,c[2]);
            }
            if (trackLPE) {
                auto cs = static_cast<SpectralSpectrum::ColorSpace>(colorSpace);
                auto write3 = [&](float* buf, float ax, float ay, float az) {
                    if (!buf) return;
                    GfVec3f c = SpectralSpectrum::XYZtoRGB(ax*invN, ay*invN, az*invN, cs);
                    buf[i*3+0] = std::max(0.f,c[0]); buf[i*3+1] = std::max(0.f,c[1]); buf[i*3+2] = std::max(0.f,c[2]);
                };
                write3(aovs->diffuseDirect,   accDiffDirX[i], accDiffDirY[i], accDiffDirZ[i]);
                write3(aovs->specularDirect,  accSpecDirX[i], accSpecDirY[i], accSpecDirZ[i]);
                write3(aovs->diffuseIndirect, accDiffIndX[i], accDiffIndY[i], accDiffIndZ[i]);
                write3(aovs->specularIndirect,accSpecIndX[i], accSpecIndY[i], accSpecIndZ[i]);
                write3(aovs->transmission,    accTransX[i],   accTransY[i],   accTransZ[i]);
            }
        }

        // ---- Ambient occlusion pass ----
        if (aoOut && camera.aoSamples > 0) {
            const int aoSpp = camera.aoSamples;
            const float aoRadius = camera.aoRadius;

            std::for_each(
                std::execution::par_unseq,
                rows.begin(), rows.end(),
                [&](unsigned int imageY)
                {
                    for (unsigned int imageX = 0; imageX < W; ++imageX) {
                        size_t pixIdx = imageY * W + imageX;

                        // Trace primary ray to get hit
                        GfRay ray = _MakeRay(camera, imageX, imageY, 0.5f, 0.5f);
                        SpectralBVH::Hit hit = bvh.Intersect(ray, 0.f);

                        if (!hit.valid()) {
                            aoOut[pixIdx] = 1.f; // no geometry = fully unoccluded
                            continue;
                        }

                        // Interpolate normal
                        float bw = 1.f - hit.u - hit.v;
                        GfVec3f N = hit.tri->n0 * bw + hit.tri->n1 * float(hit.u) + hit.tri->n2 * float(hit.v);
                        float nlen = N.GetLength();
                        if (nlen > 1e-6f) N /= nlen;

                        GfVec3f V = GfVec3f(-ray.GetDirection()[0], -ray.GetDirection()[1], -ray.GetDirection()[2]);
                        float vlen = V.GetLength();
                        if (vlen > 1e-6f) V /= vlen;
                        if (N[0]*V[0] + N[1]*V[1] + N[2]*V[2] < 0.f) N = -N;

                        GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                        GfVec3f hitPos = GfVec3f(worldHit);
                        GfVec3f aoOrigin = hitPos + N * 0.01f;

                        // Build tangent frame
                        GfVec3f up = (std::abs(N[1]) < 0.999f) ? GfVec3f(0,1,0) : GfVec3f(1,0,0);
                        GfVec3f T = GfCross(up, N);
                        float tlen = T.GetLength();
                        if (tlen > 1e-6f) T /= tlen;
                        GfVec3f B = GfCross(N, T);

                        int unoccluded = 0;
                        unsigned int aoSeed = (imageY * W + imageX) * 7919u;

                        for (int s = 0; s < aoSpp; ++s) {
                            float u1 = _Hash(aoSeed++);
                            float u2 = _Hash(aoSeed++);

                            // Cosine-weighted hemisphere sample
                            float r = std::sqrt(u1);
                            float phi = 6.28318f * u2;
                            float x = r * std::cos(phi);
                            float y = r * std::sin(phi);
                            float z = std::sqrt(std::max(0.f, 1.f - u1));

                            GfVec3f dir = T * x + B * y + N * z;
                            float dlen = dir.GetLength();
                            if (dlen > 1e-6f) dir /= dlen;

                            GfRay aoRay;
                            aoRay.SetPointAndDirection(GfVec3d(aoOrigin), GfVec3d(dir));
                            SpectralBVH::Hit aoHit = bvh.Intersect(aoRay, 0.f);

                            if (!aoHit.valid() || aoHit.t > aoRadius) {
                                unoccluded++;
                            }
                        }

                        aoOut[pixIdx] = float(unoccluded) / float(aoSpp);
                    }
                });

            fprintf(stderr, "SpectralRender: AO pass complete (%d samples, radius=%.1f)\n",
                    aoSpp, aoRadius);
        }
    }

#else
    // Brute-force fallback
    RenderTile(scene, camera,
               0, 0,
               camera.imageWidth, camera.imageHeight,
               pixels,
               /*spp=*/1);
#endif
}

// ---------------------------------------------------------------------------
// RenderTile  — brute-force path (used when Embree is not available,
//               or for sub-tile rendering in future tiled mode)
// ---------------------------------------------------------------------------
void SpectralIntegrator::RenderTile(
    const SpectralScene&  scene,
    const SpectralCamera& camera,
    unsigned int tileX, unsigned int tileY,
    unsigned int tileW,  unsigned int tileH,
    float*       pixels,
    int          /*spp*/)
{
    // Flatten visible triangles once for cache-friendly traversal
    std::vector<const SpectralTriangle*> tris;
    tris.reserve(scene.TotalTriangles());
    for (auto& kv : scene.GetMeshes()) {
        if (!kv.second.visible) continue;
        for (auto& t : kv.second.triangles)
            tris.push_back(&t);
    }

    // Row indices for parallel dispatch
    std::vector<unsigned int> rows(tileH);
    std::iota(rows.begin(), rows.end(), 0u);

    std::for_each(
        std::execution::par_unseq,
        rows.begin(), rows.end(),
        [&](unsigned int localY)
        {
            unsigned int imageY = tileY + localY;
            for (unsigned int localX = 0; localX < tileW; ++localX) {
                unsigned int imageX = tileX + localX;

                GfRay ray = _MakeRay(camera, imageX, imageY);
                Hit  hit  = _IntersectScene(ray, tris);

                GfVec3f color;
                if (hit.valid()) {
                    color = _ShadeSmoothNormal(*hit.tri, hit.u, hit.v);
                } else {
                    color = GfVec3f(0.f);
                }

                float* px = pixels + (localY * tileW + localX) * 4;
                px[0] = color[0];
                px[1] = color[1];
                px[2] = color[2];
                px[3] = 1.0f;
            }
        });
}

// ---------------------------------------------------------------------------
// _MakeRay
// ---------------------------------------------------------------------------
GfRay SpectralIntegrator::_MakeRay(
    const SpectralCamera& cam,
    unsigned int px, unsigned int py,
    float jitterX, float jitterY)
{
    // Simple NDC mapping — aspect correction is baked into projInverse
    const double ndcX =  2.0 * (px + jitterX) / cam.imageWidth  - 1.0;
    const double ndcY = -2.0 * (py + jitterY) / cam.imageHeight + 1.0;

    GfVec4d nearNDC(ndcX, ndcY, -1.0, 1.0);
    GfVec4d farNDC (ndcX, ndcY,  1.0, 1.0);

    GfVec4d nearView = cam.projInverse * nearNDC;
    GfVec4d farView  = cam.projInverse * farNDC;

    GfVec3d nearPos(nearView[0]/nearView[3],
                    nearView[1]/nearView[3],
                    nearView[2]/nearView[3]);
    GfVec3d farPos (farView[0]/farView[3],
                    farView[1]/farView[3],
                    farView[2]/farView[3]);

    GfVec3d origin = cam.viewToWorld.Transform(nearPos);
    GfVec3d target = cam.viewToWorld.Transform(farPos);
    GfVec3d dir    = target - origin;

    // DOF: thin lens model
    if (cam.fStop > 0.f && cam.focusDistance > 0.f) {
        // Lens radius from f-stop: R = focalLength / (2 * fStop)
        // focalLength in mm, convert to world units (assume 1 unit = 1cm, so /10)
        float lensRadius = (cam.focalLength * 0.1f) / (2.f * cam.fStop);

        // Focus point: where the pinhole ray hits the focal plane
        GfVec3d forward = cam.viewToWorld.TransformDir(GfVec3d(0, 0, -1));
        forward.Normalize();
        double denom = GfDot(dir, forward);
        if (std::abs(denom) > 1e-8) {
            double tFocus = double(cam.focusDistance) / (denom / dir.GetLength());
            GfVec3d focalPoint = origin + dir.GetNormalized() * tFocus;

            // Random point on lens disk (uniform)
            unsigned int seed = (py * cam.imageWidth + px) * 7919u +
                                static_cast<unsigned int>(jitterX * 65536) * 2971u;
            float u1 = _Hash(seed);
            float u2 = _Hash(seed + 1u);
            float r = lensRadius * std::sqrt(u1);
            float theta = 6.28318f * u2;

            // Lens basis vectors from camera
            GfVec3d right = cam.viewToWorld.TransformDir(GfVec3d(1, 0, 0));
            GfVec3d up    = cam.viewToWorld.TransformDir(GfVec3d(0, 1, 0));
            right.Normalize();
            up.Normalize();

            origin = origin + right * (r * std::cos(theta)) + up * (r * std::sin(theta));
            dir = focalPoint - origin;
        }
    }

    GfRay ray;
    ray.SetEnds(origin, origin + dir);
    return ray;
}

// ---------------------------------------------------------------------------
// _IntersectScene
// ---------------------------------------------------------------------------
SpectralIntegrator::Hit SpectralIntegrator::_IntersectScene(
    const GfRay& ray,
    const std::vector<const SpectralTriangle*>& tris)
{
    Hit closest;
    for (const SpectralTriangle* tri : tris) {
        double t, u, v;
        if (_IntersectTriangle(ray, *tri, t, u, v)) {
            if (t > 1e-4 && t < closest.t) {
                closest.t   = t;
                closest.u   = u;
                closest.v   = v;
                closest.tri = tri;
            }
        }
    }
    return closest;
}

// ---------------------------------------------------------------------------
// _IntersectTriangle  — Möller–Trumbore
// ---------------------------------------------------------------------------
bool SpectralIntegrator::_IntersectTriangle(
    const GfRay& ray,
    const SpectralTriangle& tri,
    double& t_out, double& u_out, double& v_out)
{
    constexpr double kEps = 1e-8;

    const GfVec3d orig = ray.GetStartPoint();
    const GfVec3d dir  = ray.GetDirection();

    GfVec3d v0(tri.v0[0], tri.v0[1], tri.v0[2]);
    GfVec3d v1(tri.v1[0], tri.v1[1], tri.v1[2]);
    GfVec3d v2(tri.v2[0], tri.v2[1], tri.v2[2]);

    GfVec3d e1 = v1 - v0;
    GfVec3d e2 = v2 - v0;
    GfVec3d h  = GfCross(dir, e2);
    double  a  = GfDot(e1, h);

    if (std::abs(a) < kEps) return false;

    double  f = 1.0 / a;
    GfVec3d s = orig - v0;
    double  u = f * GfDot(s, h);
    if (u < 0.0 || u > 1.0) return false;

    GfVec3d q = GfCross(s, e1);
    double  v = f * GfDot(dir, q);
    if (v < 0.0 || u + v > 1.0) return false;

    double t = f * GfDot(e2, q);
    if (t < kEps) return false;

    t_out = t; u_out = u; v_out = v;
    return true;
}

// ---------------------------------------------------------------------------
// _ShadeSmoothNormal
// ---------------------------------------------------------------------------
GfVec3f SpectralIntegrator::_ShadeSmoothNormal(
    const SpectralTriangle& tri, double u, double v)
{
    float w  = float(1.0 - u - v);
    float uf = float(u);
    float vf = float(v);

    GfVec3f n = tri.n0 * w + tri.n1 * uf + tri.n2 * vf;
    float len = n.GetLength();
    if (len > 1e-6f) n /= len;

    return GfVec3f(n[0]*0.5f+0.5f, n[1]*0.5f+0.5f, n[2]*0.5f+0.5f);
}

// ---------------------------------------------------------------------------
// _SkyColor
// ---------------------------------------------------------------------------
GfVec3f SpectralIntegrator::_SkyColor(const GfVec3f& dir)
{
    float t = std::max(0.0f, std::min(1.0f, dir[1]*0.5f+0.5f));
    const GfVec3f horizon(0.72f, 0.70f, 0.68f);
    const GfVec3f zenith (0.35f, 0.55f, 0.82f);
    return horizon + (zenith - horizon) * t;
}

// ---------------------------------------------------------------------------
// _ResolveMaterial — apply texture lookups to get final material properties
// ---------------------------------------------------------------------------
static SpectralMaterial _ResolveMaterial(
    const SpectralMaterial& mat,
    const SpectralTriangle& tri,
    float baryW, float baryU, float baryV,
    const SpectralScene& scene)
{
    // Interpolate UV
    GfVec2f uv = tri.uv0 * baryW + tri.uv1 * baryU + tri.uv2 * baryV;

    SpectralMaterial resolved = mat;

    // Sample baseColor texture — blend with base color using textureBlend
    if (mat.baseColorTexId >= 0 && mat.textureBlend > 0.f) {
        const SpectralTexture* tex = scene.GetTexture(mat.baseColorTexId);
        if (tex && tex->IsValid()) {
            GfVec3f texColor = tex->Sample(uv);
            float b = mat.textureBlend;
            resolved.baseColor = mat.baseColor * (1.f - b) + texColor * b;
        }
    }

    // Sample roughness texture (stored in green channel typically)
    if (mat.roughnessTexId >= 0) {
        const SpectralTexture* tex = scene.GetTexture(mat.roughnessTexId);
        if (tex && tex->IsValid()) {
            GfVec3f val = tex->Sample(uv);
            resolved.roughness = val[1]; // green channel
        }
    }

    // Sample metallic texture (stored in blue channel typically)
    if (mat.metallicTexId >= 0) {
        const SpectralTexture* tex = scene.GetTexture(mat.metallicTexId);
        if (tex && tex->IsValid()) {
            GfVec3f val = tex->Sample(uv);
            resolved.metallic = val[2]; // blue channel
        }
    }

    return resolved;
}

// ---------------------------------------------------------------------------
// _ShadeSpectral — iterative path tracing with Disney BSDF
// ---------------------------------------------------------------------------
float SpectralIntegrator::_ShadeSpectral(
    const SpectralTriangle& tri, double u, double v, float lambda,
    const SpectralMaterial& mat, const SpectralScene& scene,
    const GfVec3f& hitPos, const GfVec3f& rayDir, int maxBounces,
    unsigned int& rngSeed, const SpectralBVH& bvh, float rayTime,
    ShadeComponents* comps, const SpectralPhotonMap* photonMap,
    float gatherRadius)
{
    float w  = float(1.0 - u - v);
    float uf = float(u);
    float vf = float(v);

    GfVec3f N = tri.n0 * w + tri.n1 * uf + tri.n2 * vf;
    float len = N.GetLength();
    if (len > 1e-6f) N /= len;

    // V = direction toward camera
    GfVec3f V = GfVec3f(-rayDir[0], -rayDir[1], -rayDir[2]);
    len = V.GetLength();
    if (len > 1e-6f) V /= len;

    // Bump mapping: perturb normal from height map
    if (mat.bumpMapTexId >= 0 && mat.bumpStrength > 0.f) {
        const SpectralTexture* bumpTex = scene.GetTexture(mat.bumpMapTexId);
        if (bumpTex && bumpTex->IsValid()) {
            GfVec2f hitUV = tri.uv0 * w + tri.uv1 * uf + tri.uv2 * vf;

            // Tangent frame from UV gradients
            GfVec3f edge1 = tri.v1 - tri.v0;
            GfVec3f edge2 = tri.v2 - tri.v0;
            GfVec2f dUV1 = tri.uv1 - tri.uv0;
            GfVec2f dUV2 = tri.uv2 - tri.uv0;
            float det = dUV1[0]*dUV2[1] - dUV1[1]*dUV2[0];
            GfVec3f T, B;
            if (std::abs(det) > 1e-8f) {
                float invDet = 1.f / det;
                T = (edge1 * dUV2[1] - edge2 * dUV1[1]) * invDet;
                B = (edge2 * dUV1[0] - edge1 * dUV2[0]) * invDet;
            } else {
                GfVec3f up = (std::abs(N[1]) < 0.999f) ? GfVec3f(0,1,0) : GfVec3f(1,0,0);
                T = GfCross(up, N); B = GfCross(N, T);
            }
            float tl = T.GetLength(); if (tl > 1e-6f) T /= tl;
            float bl = B.GetLength(); if (bl > 1e-6f) B /= bl;

            // Central differences on height map
            float eps = 0.002f;
            float hC = bumpTex->Sample(hitUV)[0];
            float hR = bumpTex->Sample(hitUV + GfVec2f(eps, 0.f))[0];
            float hU = bumpTex->Sample(hitUV + GfVec2f(0.f, eps))[0];
            float dhdU = (hR - hC) / eps * mat.bumpStrength;
            float dhdV = (hU - hC) / eps * mat.bumpStrength;

            // Perturb normal
            N = GfVec3f(N[0] - dhdU*T[0] - dhdV*B[0],
                        N[1] - dhdU*T[1] - dhdV*B[1],
                        N[2] - dhdU*T[2] - dhdV*B[2]);
            len = N.GetLength();
            if (len > 1e-6f) N /= len;
        }
    }

    // Ensure N faces the camera (flip if back-facing)
    float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
    if (NdotV < 0.f) N = -N;

    // Resolve textures at this hit point
    SpectralMaterial resolvedMat = _ResolveMaterial(mat, tri, w, uf, vf, scene);

    float radiance = 0.f;

    // ---- Direct lighting with MIS ----
    if (scene.HasLights()) {
        for (const SpectralLight& light : scene.GetLights()) {
            // Strategy 1: Light sampling (NEE)
            float su1 = _Hash(rngSeed++);
            float su2 = _Hash(rngSeed++);
            GfVec3f L = light.SampleDirection(hitPos, su1, su2, N);

            // Shadow ray — trace through glass (transparent shadows)
            // Matches GPU: skip glass, block on opaque, check light distance
            bool inShadow = false;
            float shadowTransmit = 1.f;
            GfVec3f shadowOrigin = hitPos + N * 0.01f;
            for (int sb = 0; sb < 8; ++sb) {
                GfRay shadowRay;
                shadowRay.SetPointAndDirection(GfVec3d(shadowOrigin), GfVec3d(L));
                SpectralBVH::Hit shadowHit = bvh.Intersect(shadowRay, rayTime);
                if (!shadowHit.valid()) break;  // reached light

                const SpectralMaterial& sMat = scene.GetMaterial(shadowHit.tri->materialId);

                // Glass: pass through
                if (sMat.opacity < 0.99f && sMat.metallic < 0.5f) {
                    shadowTransmit *= (1.f - sMat.opacity) * 0.95f;
                    if (shadowTransmit < 0.01f) { inShadow = true; break; }
                    shadowOrigin = GfVec3f(GfVec3d(shadowOrigin) + shadowHit.t * GfVec3d(L)) + L * 0.02f;
                    continue;
                }

                // Opaque: check distance
                if (light.type == SpectralLight::Type::Distant ||
                    light.type == SpectralLight::Type::Dome) {
                    inShadow = true;
                } else {
                    float lightDist = (light.position - hitPos).GetLength();
                    inShadow = (shadowHit.t < lightDist);
                }
                break;
            }

            if (!inShadow) {
                float bsdf = SpectralBSDF::Evaluate(resolvedMat, N, V, L, lambda);
                float lightRad = (light.type == SpectralLight::Type::Dome)
                    ? light.EnvironmentEmission(L, lambda)
                    : light.SpectralEmission(lambda);
                float atten = light.Attenuation(hitPos);

                float pdfLight = light.SamplePdf(hitPos, L, N);
                float pdfBsdf  = SpectralBSDF::Pdf(resolvedMat, N, V, L);
                float misW = (pdfLight > 0.f)
                    ? SpectralBSDF::MISWeight(pdfLight, pdfBsdf)
                    : 1.f;

                float contrib = bsdf * lightRad * atten * misW * shadowTransmit;
                radiance += contrib;
                if (comps) {
                    comps->direct += contrib;
                    // LPE decomposition: split by metallic
                    float specFrac = resolvedMat.metallic + (1.f - resolvedMat.metallic) * std::pow(1.f - std::abs(N[0]*V[0]+N[1]*V[1]+N[2]*V[2]), 5.f);
                    comps->specularDirect += contrib * specFrac;
                    comps->diffuseDirect  += contrib * (1.f - specFrac);
                }
            }
        }
    }

    // ---- Spectral caustics via photon map gathering ----
    // Photon map is pre-built from lights through glass (BuildPhotonMap).
    // Each photon carries its wavelength. Gathering uses a spectral kernel
    // to match photons to the current ray's wavelength — different wavelengths
    // land at different positions after dispersion, creating the rainbow.
    if (photonMap && photonMap->PhotonCount() > 0) {
        float caustic = photonMap->GatherCaustic(hitPos, N, V, gatherRadius, lambda);
        if (caustic > 0.f) {
            radiance += caustic;
            if (comps) comps->direct += caustic;
        }
    }

    float emitVal = resolvedMat.SpectralEmission(lambda);
    radiance += emitVal;
    if (comps) comps->emission += emitVal;

    // ---- Diffraction grating ----
    // Grating equation: d(sinθ_i + sinθ_m) = mλ
    // Creates rainbow iridescence from periodic surface structure.
    if (resolvedMat.gratingSpacing > 0.f && resolvedMat.gratingStrength > 0.f) {
        float d = resolvedMat.gratingSpacing;  // μm
        float lambdaUm = lambda * 0.001f;      // nm → μm

        // Incident angle
        float cosI = std::abs(N[0]*V[0] + N[1]*V[1] + N[2]*V[2]);
        float sinI = std::sqrt(std::max(0.f, 1.f - cosI*cosI));

        // Sum first 3 diffraction orders
        float diffracted = 0.f;
        for (int m = 1; m <= 3; ++m) {
            float sinM = sinI - float(m) * lambdaUm / d;
            if (sinM > -1.f && sinM < 1.f) {
                float cosM = std::sqrt(1.f - sinM*sinM);
                // Intensity falls with order
                float orderIntensity = 1.f / float(m * m);
                diffracted += cosM * orderIntensity;
            }
        }

        // Blend with direct lighting
        float gratingContrib = diffracted * resolvedMat.gratingStrength;
        if (scene.HasLights()) {
            for (const SpectralLight& light : scene.GetLights()) {
                float lightRad = (light.type == SpectralLight::Type::Dome)
                    ? light.EnvironmentEmission(V, lambda)
                    : light.SpectralEmission(lambda);
                float atten = light.Attenuation(hitPos);
                radiance += gratingContrib * lightRad * atten * 0.2f;
                if (comps) comps->direct += gratingContrib * lightRad * atten * 0.2f;
            }
        }
    }

    // ---- Fluorescence ----
    // Material absorbs at UV/blue wavelength, re-emits at visible wavelength.
    // Only contributes when the ray's wavelength is near the absorption band.
    if (resolvedMat.fluorStrength > 0.f && resolvedMat.fluorAbsorb > 0.f) {
        float absCenter = resolvedMat.fluorAbsorb;
        float emCenter = resolvedMat.fluorEmit;
        float strength = resolvedMat.fluorStrength;

        // Absorption probability: Gaussian around absorption center (30nm bandwidth)
        float dAbs = lambda - absCenter;
        float absProb = std::exp(-dAbs * dAbs / (2.f * 30.f * 30.f));

        // Emission spectrum: Gaussian around emission center (40nm bandwidth)
        float dEm = lambda - emCenter;
        float emSpectrum = std::exp(-dEm * dEm / (2.f * 40.f * 40.f));

        // Fluorescence adds emission at the emission wavelength
        // weighted by how much absorption happens at this ray's wavelength
        // Stokes shift: emCenter > absCenter (energy loss)
        float fluorContrib = absProb * emSpectrum * strength * 0.5f;

        // Add ambient light absorption driving the fluorescence
        if (scene.HasLights()) {
            for (const SpectralLight& light : scene.GetLights()) {
                float lightRad = light.SpectralEmission(absCenter);
                float atten = light.Attenuation(hitPos);
                radiance += fluorContrib * lightRad * atten;
                if (comps) comps->emission += fluorContrib * lightRad * atten;
            }
        }
    }

    // ---- Subsurface scattering (spectral random walk) ----
    // Light enters the surface, scatters inside, exits nearby.
    // Wavelength-dependent mean free path: red scatters further in skin.
    if (resolvedMat.sssRadius > 0.f && resolvedMat.sssColor[0] + resolvedMat.sssColor[1] + resolvedMat.sssColor[2] > 0.f) {
        // Spectral mean free path from scatter colour
        auto gauss = [](float l, float c, float s) { return std::exp(-(l-c)*(l-c)/(2.f*s*s)); };
        float rB = gauss(lambda, 630.f, 30.f);
        float gB = gauss(lambda, 532.f, 30.f);
        float bB = gauss(lambda, 460.f, 25.f);
        float scatterAtLambda = resolvedMat.sssColor[0]*rB + resolvedMat.sssColor[1]*gB + resolvedMat.sssColor[2]*bB;
        scatterAtLambda = std::max(0.01f, scatterAtLambda);
        float mfp = resolvedMat.sssRadius * scatterAtLambda;

        // Random walk: scatter below surface, exit nearby
        GfVec3f walkPos = hitPos;
        float walkThroughput = 1.f;
        const int maxWalkSteps = 16;

        for (int step = 0; step < maxWalkSteps; ++step) {
            // Random direction (isotropic)
            float wu1 = _Hash(rngSeed++);
            float wu2 = _Hash(rngSeed++);
            float wz = 1.f - 2.f * wu1;
            float wr = std::sqrt(std::max(0.f, 1.f - wz*wz));
            float wphi = 6.28318f * wu2;
            GfVec3f walkDir(wr*std::cos(wphi), wr*std::sin(wphi), wz);

            // Step distance: exponential distribution with mean = mfp
            float stepDist = -mfp * std::log(std::max(1e-8f, _Hash(rngSeed++)));

            walkPos = GfVec3f(walkPos[0] + walkDir[0]*stepDist,
                              walkPos[1] + walkDir[1]*stepDist,
                              walkPos[2] + walkDir[2]*stepDist);

            // Check if we've exited the surface
            GfRay exitRay;
            exitRay.SetPointAndDirection(GfVec3d(walkPos), GfVec3d(N));
            SpectralBVH::Hit exitHit = bvh.Intersect(exitRay, rayTime);

            if (!exitHit.valid() || exitHit.t > mfp * 0.5f) {
                // Exited — add diffuse lighting at exit point
                if (scene.HasLights()) {
                    for (const SpectralLight& light : scene.GetLights()) {
                        float su1 = _Hash(rngSeed++);
                        float su2 = _Hash(rngSeed++);
                        GfVec3f L = light.SampleDirection(walkPos, su1, su2, N);
                        float NdotL = std::max(0.f, N[0]*L[0] + N[1]*L[1] + N[2]*L[2]);
                        if (NdotL > 0.f) {
                            float lightRad = light.SpectralEmission(lambda);
                            float atten = light.Attenuation(walkPos);
                            float sssContrib = walkThroughput * NdotL * lightRad * atten / 3.14159f;
                            radiance += sssContrib;
                            if (comps) comps->indirect += sssContrib;
                        }
                    }
                }
                break;
            }

            // Attenuate per step
            walkThroughput *= 0.85f;
            if (walkThroughput < 0.01f) break;
        }
    }

    // ---- Bounce rays ----
    if (maxBounces <= 0) return radiance;

    float pathThroughput = 1.f;
    GfVec3f bounceOrigin = hitPos;
    GfVec3f bounceN = N;
    GfVec3f bounceV = V;
    SpectralMaterial resolvedBounceMat = resolvedMat;
    const SpectralMaterial* bounceMat = &resolvedBounceMat;
    bool isEntering = true;
    const SpectralMaterial* insideVolumeMat = nullptr;  // Beer-Lambert tracking
    float pathMinRoughness = 0.f;  // path regularization for rough glass
    int firstBounceType = 0;  // 0=none, 1=diffuse, 2=specular, 3=transmission
    int refractionCount = 0;  // track glass bounces

    for (int bounce = 0; bounce < maxBounces; ++bounce) {
        // Russian roulette after bounce 1
        if (bounce >= 1) {
            float rrProb = std::min(0.95f, pathThroughput);
            if (_Hash(rngSeed++) > rrProb) break;
            pathThroughput /= rrProb;
        }

        // Path regularization: enforce minimum roughness from previous rough transmissions
        // Prevents sharp specular paths after rough refractions (firefly reduction)
        SpectralMaterial regularizedMat = *bounceMat;
        if (pathMinRoughness > regularizedMat.roughness)
            regularizedMat.roughness = pathMinRoughness;

        // Sample bounce direction
        float u1 = _Hash(rngSeed++);
        float u2 = _Hash(rngSeed++);
        float bounceThroughput;
        bool transmitted = false;
        GfVec3f bounceDir = SpectralBSDF::SampleDirection(
            regularizedMat, bounceN, bounceV, lambda, u1, u2,
            bounceThroughput, transmitted, isEntering);

        if (bounceThroughput <= 0.f) break;
        pathThroughput *= bounceThroughput;

        // Classify first bounce for LPE decomposition
        if (bounce == 0 && comps) {
            if (transmitted)
                firstBounceType = 3;  // transmission
            else if (regularizedMat.metallic > 0.5f || regularizedMat.roughness < 0.3f)
                firstBounceType = 2;  // specular
            else
                firstBounceType = 1;  // diffuse
        }

        // Beer-Lambert: update volume tracking based on transmission
        if (transmitted) {
            // Path regularization: after rough refraction, raise min roughness
            if (bounceMat->roughness > 0.05f)
                pathMinRoughness = std::max(pathMinRoughness, bounceMat->roughness * 0.5f);

            if (isEntering && bounceMat->absorptionDensity > 0.f) {
                insideVolumeMat = bounceMat;
            } else if (!isEntering) {
                insideVolumeMat = nullptr;
            }

            // Track refraction depth
            refractionCount++;
        }

        // Trace bounce ray
        GfVec3f bOffset = transmitted ? (bounceN * -0.1f) : (bounceN * 0.01f);
        GfRay bounceRay;
        bounceRay.SetPointAndDirection(
            GfVec3d(bounceOrigin + bOffset),
            GfVec3d(bounceDir));

        SpectralBVH::Hit bounceHit = bvh.Intersect(bounceRay, rayTime);

        // Beer-Lambert absorption: attenuate for distance traveled inside volume
        if (insideVolumeMat && bounceHit.valid()) {
            float T = insideVolumeMat->SpectralTransmittance(lambda, float(bounceHit.t));
            pathThroughput *= T;
            if (pathThroughput < 1e-6f) break;
        }

        if (!bounceHit.valid()) {
            // Miss — check dome lights for BSDF-side MIS contribution
            if (!scene.GetLights().empty()) {
                for (const SpectralLight& light : scene.GetLights()) {
                    if (light.type != SpectralLight::Type::Dome) continue;
                    float domeRad = light.EnvironmentEmission(bounceDir, lambda);
                    if (domeRad > 0.f) {
                        float pdfBsdf  = SpectralBSDF::Pdf(*bounceMat, bounceN, bounceV, bounceDir);
                        float pdfLight = light.SamplePdf(bounceOrigin, bounceDir, bounceN);
                        float misW = (pdfBsdf > 0.f && pdfLight > 0.f)
                            ? SpectralBSDF::MISWeight(pdfBsdf, pdfLight)
                            : 1.f;
                        float contrib = pathThroughput * domeRad * misW;
                        radiance += contrib;
                        if (comps) comps->indirect += contrib;
                    }
                }
            }
            break;
        }

        // Hit surface — set up for next iteration
        const SpectralTriangle& hitTri = *bounceHit.tri;
        const SpectralMaterial& rawBounceMat = scene.GetMaterial(hitTri.materialId);

        float bw = 1.f - bounceHit.u - bounceHit.v;
        // Resolve textures at bounce hit
        resolvedBounceMat = _ResolveMaterial(
            rawBounceMat, hitTri, bw, bounceHit.u, bounceHit.v, scene);
        bounceMat = &resolvedBounceMat;
        bounceN = hitTri.n0 * bw + hitTri.n1 * bounceHit.u + hitTri.n2 * bounceHit.v;
        float nlen = bounceN.GetLength();
        if (nlen > 1e-6f) bounceN /= nlen;

        bounceV = GfVec3f(-bounceDir[0], -bounceDir[1], -bounceDir[2]);

        // Determine entering/exiting BEFORE flipping normal
        float bNdotV = bounceN[0]*bounceV[0] + bounceN[1]*bounceV[1] + bounceN[2]*bounceV[2];
        isEntering = (bNdotV > 0.f);  // normal faces camera = entering

        // Flip normal to face camera
        if (bNdotV < 0.f) bounceN = -bounceN;

        bounceOrigin = GfVec3f(
            bounceOrigin[0] + bOffset[0] + bounceDir[0] * bounceHit.t,
            bounceOrigin[1] + bOffset[1] + bounceDir[1] * bounceHit.t,
            bounceOrigin[2] + bOffset[2] + bounceDir[2] * bounceHit.t);

        // Direct lighting — skip if inside a transparent object
        bool insideTransparent = (!isEntering && bounceMat->opacity < 0.99f);
        float bounceRadiance = 0.f;
        if (scene.HasLights() && !insideTransparent) {
            for (const SpectralLight& light : scene.GetLights()) {
                float bsu1 = _Hash(rngSeed++);
                float bsu2 = _Hash(rngSeed++);
                GfVec3f L = light.SampleDirection(bounceOrigin, bsu1, bsu2, bounceN);

                bool inShadow = false;
                float shadowTransmit = 1.f;
                GfVec3f sOrig = bounceOrigin + bounceN * 0.01f;
                for (int sb = 0; sb < 8; ++sb) {
                    GfRay shadowRay;
                    shadowRay.SetPointAndDirection(GfVec3d(sOrig), GfVec3d(L));
                    SpectralBVH::Hit shadowHit = bvh.Intersect(shadowRay, rayTime);
                    if (!shadowHit.valid()) break;

                    const SpectralMaterial& sMat = scene.GetMaterial(shadowHit.tri->materialId);
                    if (sMat.opacity < 0.99f && sMat.metallic < 0.5f) {
                        shadowTransmit *= (1.f - sMat.opacity) * 0.95f;
                        if (shadowTransmit < 0.01f) { inShadow = true; break; }
                        sOrig = GfVec3f(GfVec3d(sOrig) + shadowHit.t * GfVec3d(L)) + L * 0.02f;
                        continue;
                    }

                    if (light.type == SpectralLight::Type::Distant ||
                        light.type == SpectralLight::Type::Dome) {
                        inShadow = true;
                    } else {
                        float lightDist = (light.position - bounceOrigin).GetLength();
                        inShadow = (shadowHit.t < lightDist);
                    }
                    break;
                }

                if (!inShadow) {
                    float bsdf = SpectralBSDF::Evaluate(*bounceMat, bounceN, bounceV, L, lambda);
                    float lightRad = (light.type == SpectralLight::Type::Dome)
                        ? light.EnvironmentEmission(L, lambda)
                        : light.SpectralEmission(lambda);
                    float atten = light.Attenuation(bounceOrigin);

                    float pdfLight = light.SamplePdf(bounceOrigin, L, bounceN);
                    float pdfBsdf  = SpectralBSDF::Pdf(*bounceMat, bounceN, bounceV, L);
                    float misW = (pdfLight > 0.f)
                        ? SpectralBSDF::MISWeight(pdfLight, pdfBsdf)
                        : 1.f;

                    bounceRadiance += bsdf * lightRad * atten * misW * shadowTransmit;
                }
            }
        }

        if (!insideTransparent) {
            bounceRadiance += bounceMat->SpectralEmission(lambda);
        }
        radiance += pathThroughput * bounceRadiance;
        if (comps) {
            float indContrib = pathThroughput * bounceRadiance;
            comps->indirect += indContrib;
            // LPE indirect decomposition based on first bounce type
            if (firstBounceType == 3)
                comps->transmission += indContrib;
            else if (firstBounceType == 2)
                comps->specularIndirect += indContrib;
            else
                comps->diffuseIndirect += indContrib;
        }
    }

    return radiance;
}

// ---------------------------------------------------------------------------
// _SkySpectral — spectral radiance of the sky at one wavelength
//
//   Models a simple Rayleigh-like sky: shorter wavelengths scatter
//   more strongly (blue zenith), longer wavelengths dominate at
//   the horizon (warm sunset tones).
// ---------------------------------------------------------------------------
float SpectralIntegrator::_SkySpectral(const GfVec3f& dir, float lambda)
{
    float t = std::max(0.f, std::min(1.f, dir[1] * 0.5f + 0.5f));

    // Rayleigh scattering: intensity ∝ 1/λ⁴
    // Normalise so 460nm (blue) ≈ 1.0 and 620nm (red) ≈ 0.3
    float scatter = 1.f;
    if (lambda > 400.f) {
        float ratio = 460.f / lambda;
        scatter = ratio * ratio * ratio * ratio;
    }

    // Horizon: warm broadband (high reflectance across all wavelengths)
    // Zenith:  Rayleigh-scattered (strong blue, weak red)
    float horizonRadiance = 0.7f;
    float zenithRadiance  = 0.4f * scatter;

    return horizonRadiance + (zenithRadiance - horizonRadiance) * t;
}

// ---------------------------------------------------------------------------
// _Hash — simple deterministic hash for per-pixel jitter
//   Based on Wang hash — fast, good distribution, no state needed.
// ---------------------------------------------------------------------------
float SpectralIntegrator::_Hash(unsigned int seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / float(0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// ComputeAO — ambient occlusion pass
// ---------------------------------------------------------------------------
void SpectralIntegrator::ComputeAO(
    const SpectralScene& scene,
    const SpectralCamera& camera,
    float* aoOut,
    int aoSamples,
    float aoRadius)
{
#ifdef SPECTRAL_HAS_EMBREE
    if (aoSamples <= 0 || !aoOut) return;

    SpectralBVH bvh;
    bvh.Build(scene);

    const unsigned int W = camera.imageWidth;
    const unsigned int H = camera.imageHeight;

    std::vector<unsigned int> rows(H);
    std::iota(rows.begin(), rows.end(), 0u);

    std::for_each(
        std::execution::par_unseq,
        rows.begin(), rows.end(),
        [&](unsigned int imageY)
        {
            for (unsigned int imageX = 0; imageX < W; ++imageX) {
                const size_t pixIdx = imageY * W + imageX;

                GfRay ray = _MakeRay(camera, imageX, imageY, 0.5f, 0.5f);
                SpectralBVH::Hit hit = bvh.Intersect(ray, 0.f);

                if (!hit.valid()) {
                    aoOut[pixIdx] = 1.f;
                    continue;
                }

                // Compute hit position and normal
                float w = 1.f - float(hit.u) - float(hit.v);
                GfVec3f N = hit.tri->n0 * w + hit.tri->n1 * float(hit.u) + hit.tri->n2 * float(hit.v);
                float nlen = N.GetLength();
                if (nlen > 1e-6f) N /= nlen;

                GfVec3f V = GfVec3f(-ray.GetDirection());
                float vlen = V.GetLength();
                if (vlen > 1e-6f) V /= vlen;
                if (N[0]*V[0] + N[1]*V[1] + N[2]*V[2] < 0.f) N = -N;

                GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                GfVec3f hitPos = GfVec3f(worldHit);
                GfVec3f origin = hitPos + N * 0.01f;

                // Build tangent frame
                GfVec3f up = (std::abs(N[1]) < 0.999f) ? GfVec3f(0,1,0) : GfVec3f(1,0,0);
                GfVec3f T = GfCross(up, N);
                float tlen = T.GetLength();
                if (tlen > 1e-6f) T /= tlen;
                GfVec3f B = GfCross(N, T);

                // Shoot AO rays
                int occluded = 0;
                for (int s = 0; s < aoSamples; ++s) {
                    unsigned int seed = (imageY * W + imageX) * 7919 + s * 1231;
                    float u1 = _Hash(seed);
                    float u2 = _Hash(seed + 1);

                    // Cosine-weighted hemisphere sample
                    float r = std::sqrt(u1);
                    float phi = 6.28318f * u2;
                    float x = r * std::cos(phi);
                    float y = r * std::sin(phi);
                    float z = std::sqrt(std::max(0.f, 1.f - u1));
                    GfVec3f dir = T * x + B * y + N * z;
                    float dlen = dir.GetLength();
                    if (dlen > 1e-6f) dir /= dlen;

                    GfRay aoRay;
                    aoRay.SetPointAndDirection(GfVec3d(origin), GfVec3d(dir));
                    SpectralBVH::Hit aoHit = bvh.Intersect(aoRay, 0.f);

                    if (aoHit.valid() && aoHit.t < aoRadius) {
                        occluded++;
                    }
                }

                aoOut[pixIdx] = 1.f - float(occluded) / float(aoSamples);
            }
        });

    fprintf(stderr, "SpectralIntegrator: AO pass complete (%d samples, radius %.1f)\n",
            aoSamples, aoRadius);
#endif
}

// ---------------------------------------------------------------------------
// ComputeGeometryAOVs — quick single-ray pass for N, P, UV, albedo, IDs
// ---------------------------------------------------------------------------
void SpectralIntegrator::ComputeGeometryAOVs(
    const SpectralScene& scene,
    const SpectralCamera& camera,
    float* normalOut, float* posOut, float* pRefOut, float* uvOut, float* albedoOut,
    float* objectIdOut, float* materialIdOut, float* depthOut)
{
#ifdef SPECTRAL_HAS_EMBREE
    SpectralBVH bvh;
    bvh.Build(scene);

    const unsigned int W = camera.imageWidth;
    const unsigned int H = camera.imageHeight;
    const GfMatrix4d worldToView = camera.viewToWorld.GetInverse();

    std::vector<unsigned int> rows(H);
    std::iota(rows.begin(), rows.end(), 0u);

    std::for_each(
        std::execution::par_unseq,
        rows.begin(), rows.end(),
        [&](unsigned int imageY)
        {
            for (unsigned int imageX = 0; imageX < W; ++imageX) {
                const size_t pixIdx = imageY * W + imageX;

                GfRay ray = _MakeRay(camera, imageX, imageY, 0.5f, 0.5f);
                SpectralBVH::Hit hit = bvh.Intersect(ray, 0.f);

                if (!hit.valid()) {
                    if (depthOut) depthOut[pixIdx] = 1e30f;
                    if (objectIdOut) objectIdOut[pixIdx] = 0.f;
                    if (materialIdOut) materialIdOut[pixIdx] = 0.f;
                    continue;
                }

                float w = 1.f - float(hit.u) - float(hit.v);

                // Normal
                GfVec3f N = hit.tri->n0 * w + hit.tri->n1 * float(hit.u) + hit.tri->n2 * float(hit.v);
                float nlen = N.GetLength();
                if (nlen > 1e-6f) N /= nlen;
                GfVec3f V = GfVec3f(-ray.GetDirection());
                float vlen = V.GetLength();
                if (vlen > 1e-6f) V /= vlen;
                if (N[0]*V[0]+N[1]*V[1]+N[2]*V[2] < 0.f) N = -N;

                if (normalOut) {
                    normalOut[pixIdx*3+0] = N[0];
                    normalOut[pixIdx*3+1] = N[1];
                    normalOut[pixIdx*3+2] = N[2];
                }

                // Position
                GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                GfVec3f hitPos = GfVec3f(worldHit);
                if (posOut) {
                    posOut[pixIdx*3+0] = hitPos[0];
                    posOut[pixIdx*3+1] = hitPos[1];
                    posOut[pixIdx*3+2] = hitPos[2];
                }

                // pRef (reference/undisplaced position)
                if (pRefOut) {
                    GfVec3f pRef = hit.tri->pRef0 * w + hit.tri->pRef1 * float(hit.u) + hit.tri->pRef2 * float(hit.v);
                    pRefOut[pixIdx*3+0] = pRef[0];
                    pRefOut[pixIdx*3+1] = pRef[1];
                    pRefOut[pixIdx*3+2] = pRef[2];
                }

                // UV
                if (uvOut) {
                    GfVec2f uv = hit.tri->uv0 * w + hit.tri->uv1 * float(hit.u) + hit.tri->uv2 * float(hit.v);
                    uvOut[pixIdx*2+0] = uv[0];
                    uvOut[pixIdx*2+1] = uv[1];
                }

                // Albedo
                if (albedoOut) {
                    const SpectralMaterial& mat = scene.GetMaterial(hit.tri->materialId);
                    SpectralMaterial resolved = _ResolveMaterial(mat, *hit.tri, w, float(hit.u), float(hit.v), scene);
                    albedoOut[pixIdx*3+0] = resolved.baseColor[0];
                    albedoOut[pixIdx*3+1] = resolved.baseColor[1];
                    albedoOut[pixIdx*3+2] = resolved.baseColor[2];
                }

                // Depth
                if (depthOut) {
                    GfVec3d viewHit = worldToView.Transform(worldHit);
                    depthOut[pixIdx] = static_cast<float>(-viewHit[2]);
                }

                // IDs
                if (objectIdOut) objectIdOut[pixIdx] = static_cast<float>(hit.tri->objectId);
                if (materialIdOut) materialIdOut[pixIdx] = static_cast<float>(hit.tri->materialId);
            }
        });

    fprintf(stderr, "SpectralIntegrator: geometry AOV pass complete (%dx%d)\n", W, H);
#endif
}

// ---------------------------------------------------------------------------
// BuildPhotonMap — trace photons from lights through specular surfaces
//
//   Each photon carries a single wavelength. When it refracts through
//   glass with dispersion (Abbe number), Snell's law bends each
//   wavelength differently — this naturally separates the spectrum
//   into a rainbow pattern in the photon map.
//
//   Photons are stored when they hit a diffuse surface (opacity > 0.9,
//   metallic < 0.5). This captures Light→Specular→Diffuse caustic paths.
// ---------------------------------------------------------------------------
void SpectralIntegrator::BuildPhotonMap(
    const SpectralScene& scene,
    SpectralPhotonMap& photonMap,
    int numPhotons,
    int maxBounces)
{
#ifdef SPECTRAL_HAS_EMBREE
    SpectralBVH bvh;
    bvh.Build(scene);

    photonMap.Clear();

    if (!scene.HasLights() || scene.TotalTriangles() == 0) return;

    const auto& lights = scene.GetLights();
    int photonsPerLight = numPhotons / std::max(1, static_cast<int>(lights.size()));

    for (size_t li = 0; li < lights.size(); ++li) {
        const SpectralLight& light = lights[li];
        if (light.type == SpectralLight::Type::Dome) continue;  // skip env lights for caustics

        float lightPower = light.EffectiveIntensity();

        for (int pi = 0; pi < photonsPerLight; ++pi) {
            unsigned int seed = static_cast<unsigned int>(li * 1000003u + pi * 6571u);

            // Random wavelength for this photon
            float wu = _Hash(seed++);
            float lambda = 380.f + wu * 400.f;

            // Emit photon from light
            GfVec3f photonOrigin = light.position;
            GfVec3f photonDir;

            if (light.type == SpectralLight::Type::Distant) {
                // Parallel rays — jitter origin on a disk perpendicular to direction
                photonDir = GfVec3f(-light.direction[0], -light.direction[1], -light.direction[2]);
                float dlen = photonDir.GetLength();
                if (dlen > 1e-6f) photonDir /= dlen;
                // Place origin far away along direction
                photonOrigin = GfVec3f(0,0,0) - photonDir * 1000.f;
                // Jitter on disk
                GfVec3f up = (std::abs(photonDir[1]) < 0.999f) ? GfVec3f(0,1,0) : GfVec3f(1,0,0);
                GfVec3f T = GfCross(up, photonDir); T.Normalize();
                GfVec3f B = GfCross(photonDir, T);
                float r = 50.f * std::sqrt(_Hash(seed++));
                float phi = 6.28318f * _Hash(seed++);
                photonOrigin = photonOrigin + T * (r * std::cos(phi)) + B * (r * std::sin(phi));
            } else {
                // Point/sphere: random direction on sphere
                float u1 = _Hash(seed++);
                float u2 = _Hash(seed++);
                float z = 1.f - 2.f * u1;
                float r = std::sqrt(std::max(0.f, 1.f - z * z));
                float phi = 6.28318f * u2;
                photonDir = GfVec3f(r * std::cos(phi), r * std::sin(phi), z);
            }

            float photonPower = lightPower * light.SpectralEmission(lambda)
                              / float(photonsPerLight);

            // Trace photon through scene
            GfVec3f pos = photonOrigin;
            GfVec3f dir = photonDir;
            float power = photonPower;
            bool hitSpecular = false;  // only store after at least one specular bounce

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                GfRay ray;
                ray.SetPointAndDirection(GfVec3d(pos), GfVec3d(dir));
                SpectralBVH::Hit hit = bvh.Intersect(ray, 0.f);

                if (!hit.valid()) break;

                const SpectralMaterial& mat = scene.GetMaterial(hit.tri->materialId);
                GfVec3d worldHit = GfVec3d(pos) + hit.t * GfVec3d(dir);
                GfVec3f hitPos = GfVec3f(worldHit);

                float bw = 1.f - float(hit.u) - float(hit.v);
                GfVec3f N = hit.tri->n0 * bw + hit.tri->n1 * float(hit.u) + hit.tri->n2 * float(hit.v);
                float nlen = N.GetLength();
                if (nlen > 1e-6f) N /= nlen;

                // Check if surface is specular (glass) or diffuse
                bool isSpecular = (mat.opacity < 0.99f && mat.metallic < 0.5f)
                               || mat.roughness < 0.1f;
                bool isDiffuse = !isSpecular;

                if (isDiffuse && hitSpecular) {
                    // Store caustic photon — this is a L*S+D path
                    SpectralPhoton p;
                    p.position = hitPos;
                    p.direction = dir;
                    p.wavelength = lambda;
                    p.power = power;
                    photonMap.Store(p);
                    break;  // photon absorbed
                }

                if (isDiffuse && !hitSpecular) {
                    break;  // direct diffuse hit, no caustic
                }

                // Specular interaction — reflect or refract
                hitSpecular = true;

                GfVec3f V = GfVec3f(-dir[0], -dir[1], -dir[2]);
                float vlen = V.GetLength();
                if (vlen > 1e-6f) V /= vlen;

                float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
                bool entering = (NdotV > 0.f);
                if (NdotV < 0.f) N = -N;

                // IOR with dispersion
                float ior = mat.ior;
                if (mat.abbeNumber > 0.f) {
                    float dispersion = (ior - 1.f) / mat.abbeNumber;
                    float dLambda = (587.6f - lambda) / (656.3f - 486.1f);
                    ior = ior + dispersion * dLambda;
                }

                float eta = entering ? (1.f / ior) : ior;
                float cosI = std::abs(V[0]*N[0] + V[1]*N[1] + V[2]*N[2]);

                // Fresnel
                float sinT2 = eta * eta * (1.f - cosI * cosI);
                float F;
                if (sinT2 > 1.f) {
                    F = 1.f;  // TIR
                } else {
                    float cosT = std::sqrt(1.f - sinT2);
                    float rS = (cosI - ior * cosT) / (cosI + ior * cosT);
                    float rP = (ior * cosI - cosT) / (ior * cosI + cosT);
                    F = 0.5f * (rS * rS + rP * rP);
                }

                GfVec3f geoN = entering ? N : -N;

                if (_Hash(seed++) < F) {
                    // Reflect
                    float VdotN = V[0]*N[0] + V[1]*N[1] + V[2]*N[2];
                    dir = GfVec3f(N[0] * 2.f * VdotN - V[0],
                                  N[1] * 2.f * VdotN - V[1],
                                  N[2] * 2.f * VdotN - V[2]);
                    float dl = dir.GetLength();
                    if (dl > 1e-6f) dir /= dl;
                    pos = hitPos + N * 0.01f;
                } else {
                    // Refract
                    float cosT = std::sqrt(std::max(0.f, 1.f - sinT2));
                    dir = GfVec3f(-V[0] * eta + geoN[0] * (eta * cosI - cosT),
                                  -V[1] * eta + geoN[1] * (eta * cosI - cosT),
                                  -V[2] * eta + geoN[2] * (eta * cosI - cosT));
                    float dl = dir.GetLength();
                    if (dl > 1e-6f) dir /= dl;
                    pos = hitPos - N * 0.01f;  // offset into surface
                }

                // Russian roulette
                if (bounce >= 2) {
                    if (_Hash(seed++) > 0.8f) break;
                    power /= 0.8f;
                }
            }
        }
    }

    // Build spatial grid for fast gathering — cell size = gather radius / 2
    float cellSize = std::max(0.1f, 0.25f);  // default grid resolution
    photonMap.Build(cellSize);

    fprintf(stderr, "SpectralIntegrator: caustic photon map — %zu photons stored from %zu lights\n",
            photonMap.PhotonCount(), lights.size());
#endif
}

// ---------------------------------------------------------------------------
// GPU render path (Phase 3)
// ---------------------------------------------------------------------------
#ifdef SPECTRAL_HAS_OPTIX

#include "SpectralGPUKernel_ptx.h"   // embedded PTX string

// Lazy-initialized singleton GPU context
static SpectralGPU* _GetGPU()
{
    static SpectralGPU* s_gpu = nullptr;
    static bool s_tried = false;

    if (!s_tried) {
        s_tried = true;
        s_gpu = new SpectralGPU();
        if (!s_gpu->Initialize(std::string(kSpectralGPUKernelPTX))) {
            fprintf(stderr, "SpectralIntegrator: GPU init failed, falling back to CPU\n");
            delete s_gpu;
            s_gpu = nullptr;
        } else {
            fprintf(stderr, "SpectralIntegrator: GPU initialized successfully\n");
        }
    }
    return s_gpu;
}

bool SpectralIntegrator::IsGPUAvailable()
{
    return _GetGPU() != nullptr;
}

void SpectralIntegrator::RenderFrameGPU(
    const SpectralScene&  scene,
    const SpectralCamera& camera,
    float*                pixels,
    int                   spp,
    float*                depthOut,
    int                   maxBounces,
    int                   colorSpace,
    const SpectralVolume* const* volumes,
    int                   numVolumes)
{
    // Phase 13: GPU handles multi-volume natively (up to 8)
    if (numVolumes > SPECTRAL_MAX_GPU_VOLUMES) {
        fprintf(stderr, "SpectralIntegrator: %d volumes exceeds GPU max (%d), capping\n",
                numVolumes, SPECTRAL_MAX_GPU_VOLUMES);
        numVolumes = SPECTRAL_MAX_GPU_VOLUMES;
    }

    SpectralGPU* gpu = _GetGPU();
    if (!gpu) {
        fprintf(stderr, "SpectralIntegrator: GPU unavailable, using CPU\n");
        RenderFrame(scene, camera, pixels, spp, depthOut, maxBounces,
                    nullptr, nullptr, nullptr, nullptr, nullptr, 0.5f, colorSpace,
                    volumes, numVolumes);
        return;
    }

    if (!gpu->BuildAccel(scene)) {
        fprintf(stderr, "SpectralIntegrator: GPU accel build failed, using CPU\n");
        RenderFrame(scene, camera, pixels, spp, depthOut, maxBounces,
                    nullptr, nullptr, nullptr, nullptr, nullptr, 0.5f, colorSpace,
                    volumes, numVolumes);
        return;
    }

    if (!gpu->Render(camera, camera.imageWidth, camera.imageHeight,
                     pixels, depthOut, spp, maxBounces, colorSpace,
                     volumes, numVolumes)) {
        fprintf(stderr, "SpectralIntegrator: GPU render failed, using CPU\n");
        RenderFrame(scene, camera, pixels, spp, depthOut, maxBounces,
                    nullptr, nullptr, nullptr, nullptr, nullptr, 0.5f, colorSpace,
                    volumes, numVolumes);
        return;
    }

    fprintf(stderr, "SpectralIntegrator: GPU render complete (%dx%d) %d volume(s)\n",
            camera.imageWidth, camera.imageHeight, numVolumes);
}

void SpectralIntegrator::DenoiseGPU(
    unsigned int width, unsigned int height, float* pixels)
{
    SpectralGPU* gpu = _GetGPU();
    if (!gpu) {
        fprintf(stderr, "SpectralIntegrator: GPU not available for denoising\n");
        return;
    }
    gpu->Denoise(width, height, pixels);
}

#endif // SPECTRAL_HAS_OPTIX

PXR_NAMESPACE_CLOSE_SCOPE
