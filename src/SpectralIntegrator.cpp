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
    float*                materialIdOut)
{
#ifdef SPECTRAL_HAS_EMBREE
    SpectralBVH bvh;
    bvh.Build(scene);

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
                        GfVec3f dir = GfVec3f(ray.GetDirection());
                        float len = dir.GetLength();
                        if (len > 1e-6f) dir /= len;
                        color = _SkyColor(dir);
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
                    float X = 0.f, Y = 0.f, Z = 0.f;
                    float minDepth = 1e30f;
                    int firstObjectId = 0, firstMaterialId = 0;
                    bool gotFirstHit = false;

                    for (int s = 0; s < spp; ++s) {
                        unsigned int seed = (imageY * W + imageX) * 1031 + s * 6571;

                        float jx = _Hash(seed);
                        float jy = _Hash(seed + 1);

                        float wu = (float(s) + _Hash(seed + 2)) / float(spp);
                        float lambda = SpectralSpectrum::SampleWavelength(wu);

                        // Random time for motion blur [shutterOpen, shutterClose]
                        float rayTime = camera.shutterOpen +
                            _Hash(seed + 3) * (camera.shutterClose - camera.shutterOpen);

                        GfRay ray = _MakeRay(camera, imageX, imageY, jx, jy);
                        SpectralBVH::Hit hit = bvh.Intersect(ray, rayTime);

                        float radiance;
                        if (hit.valid()) {
                            const SpectralMaterial& mat = scene.GetMaterial(hit.tri->materialId);
                            GfVec3d worldHit = ray.GetStartPoint() + hit.t * ray.GetDirection();
                            GfVec3f hitPos = GfVec3f(worldHit);
                            GfVec3f rayDir = GfVec3f(ray.GetDirection());
                            unsigned int bounceSeed = seed + 100u;
                            radiance = _ShadeSpectral(
                                *hit.tri,
                                static_cast<double>(hit.u),
                                static_cast<double>(hit.v),
                                lambda, mat, scene, hitPos, rayDir,
                                maxBounces, bounceSeed, bvh, rayTime);
                            // Camera-space Z for this sample
                            GfVec3d viewHit = worldToView.Transform(worldHit);
                            float camZ = static_cast<float>(-viewHit[2]);
                            if (camZ < minDepth) minDepth = camZ;
                            if (!gotFirstHit) {
                                firstObjectId = hit.tri->objectId;
                                firstMaterialId = hit.tri->materialId;
                                gotFirstHit = true;
                            }
                        } else {
                            GfVec3f dir = GfVec3f(ray.GetDirection());
                            float len = dir.GetLength();
                            if (len > 1e-6f) dir /= len;
                            radiance = _SkySpectral(dir, lambda);
                        }

                        GfVec3f xyz = SpectralSpectrum::RadianceToXYZ(radiance, lambda);
                        X += xyz[0];
                        Y += xyz[1];
                        Z += xyz[2];
                    }

                    float invSpp = 1.f / float(spp);
                    GfVec3f rgb = SpectralSpectrum::XYZtoLinearRGB(
                        X * invSpp, Y * invSpp, Z * invSpp);

                    float* px = pixels + pixIdx * 4;
                    px[0] = std::max(0.f, rgb[0]);
                    px[1] = std::max(0.f, rgb[1]);
                    px[2] = std::max(0.f, rgb[2]);
                    px[3] = 1.0f;

                    if (depthOut) depthOut[pixIdx] = minDepth;
                    if (objectIdOut) objectIdOut[pixIdx] = static_cast<float>(firstObjectId);
                    if (materialIdOut) materialIdOut[pixIdx] = static_cast<float>(firstMaterialId);
                }
            }
        });

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
                    GfVec3f dir = GfVec3f(ray.GetDirection());
                    float len = dir.GetLength();
                    if (len > 1e-6f) dir /= len;
                    color = _SkyColor(dir);
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
    GfVec3d dir    = cam.viewToWorld.Transform(farPos) - origin;

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

    // Sample baseColor texture
    if (mat.baseColorTexId >= 0) {
        const SpectralTexture* tex = scene.GetTexture(mat.baseColorTexId);
        if (tex && tex->IsValid()) {
            resolved.baseColor = tex->Sample(uv);
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
    unsigned int& rngSeed, const SpectralBVH& bvh, float rayTime)
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

    // Ensure N faces the camera (flip if back-facing)
    float NdotV = N[0]*V[0] + N[1]*V[1] + N[2]*V[2];
    if (NdotV < 0.f) N = -N;

    // Resolve textures at this hit point
    SpectralMaterial resolvedMat = _ResolveMaterial(mat, tri, w, uf, vf, scene);

    float radiance = 0.f;

    // ---- Direct lighting from explicit lights ----
    if (scene.HasLights()) {
        for (const SpectralLight& light : scene.GetLights()) {
            GfVec3f L = light.DirectionFrom(hitPos);

            // Shadow ray — offset origin along normal to clear surface
            bool inShadow = false;
            GfVec3f shadowOrigin = hitPos + N * 0.01f;
            GfRay shadowRay;
            shadowRay.SetEnds(GfVec3d(shadowOrigin),
                GfVec3d(shadowOrigin) + GfVec3d(L) * 10000.0);
            SpectralBVH::Hit shadowHit = bvh.Intersect(shadowRay, rayTime);

            if (shadowHit.valid()) {
                // Check if the shadow hit is a legitimate blocker:
                // Skip backface hits (ray passing through the same convex object)
                GfVec3f shadowN = shadowHit.tri->faceNormal;
                float hitFacing = shadowN[0]*L[0] + shadowN[1]*L[1] + shadowN[2]*L[2];

                // If the shadow hit's normal faces toward the light (hitFacing > 0),
                // it's a backface of the same object — skip it.
                // A real blocker's face normal points against the light direction.
                if (hitFacing < 0.f) {
                    if (light.type == SpectralLight::Type::Distant ||
                        light.type == SpectralLight::Type::Dome) {
                        inShadow = true;
                    } else {
                        float lightDist = (light.position - hitPos).GetLength();
                        inShadow = (shadowHit.t < lightDist);
                    }
                }
            }

            if (!inShadow) {
                float bsdf = SpectralBSDF::Evaluate(resolvedMat, N, V, L, lambda);
                float lightRad = light.SpectralEmission(lambda);
                float atten = light.Attenuation(hitPos);
                radiance += bsdf * lightRad * atten;
            }
        }
        // Sky fill
        auto skyFn = [](const GfVec3f& dir, float lam) -> float {
            return _SkySpectral(dir, lam);
        };
        radiance += SpectralBSDF::EvaluateSkylighting(resolvedMat, N, V, lambda, skyFn) * 0.05f;
    } else {
        // No explicit lights — full sky lighting
        auto skyFn = [](const GfVec3f& dir, float lam) -> float {
            return _SkySpectral(dir, lam);
        };
        radiance = SpectralBSDF::EvaluateSkylighting(resolvedMat, N, V, lambda, skyFn);
    }

    radiance += resolvedMat.SpectralEmission(lambda);

    // ---- Bounce rays ----
    if (maxBounces <= 0) return radiance;

    float pathThroughput = 1.f;
    GfVec3f bounceOrigin = hitPos;
    GfVec3f bounceN = N;
    GfVec3f bounceV = V;
    SpectralMaterial resolvedBounceMat = resolvedMat;
    const SpectralMaterial* bounceMat = &resolvedBounceMat;

    for (int bounce = 0; bounce < maxBounces; ++bounce) {
        // Russian roulette after bounce 1
        if (bounce >= 1) {
            float rrProb = std::min(0.95f, pathThroughput);
            if (_Hash(rngSeed++) > rrProb) break;
            pathThroughput /= rrProb;
        }

        // Sample bounce direction
        float u1 = _Hash(rngSeed++);
        float u2 = _Hash(rngSeed++);
        float bounceThroughput;
        GfVec3f bounceDir = SpectralBSDF::SampleDirection(
            *bounceMat, bounceN, bounceV, lambda, u1, u2, bounceThroughput);

        if (bounceThroughput <= 0.f) break;
        pathThroughput *= bounceThroughput;

        // Trace bounce ray
        GfVec3f bOffset = bounceN * 0.01f;
        GfRay bounceRay;
        bounceRay.SetEnds(
            GfVec3d(bounceOrigin + bOffset),
            GfVec3d(bounceOrigin + bOffset) + GfVec3d(bounceDir) * 10000.0);

        SpectralBVH::Hit bounceHit = bvh.Intersect(bounceRay, rayTime);

        if (!bounceHit.valid()) {
            // Hit sky
            GfVec3f dir = GfVec3f(bounceRay.GetDirection());
            float dlen = dir.GetLength();
            if (dlen > 1e-6f) dir /= dlen;
            radiance += pathThroughput * _SkySpectral(dir, lambda);
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

        // Flip normal if back-facing
        float bNdotV = bounceN[0]*bounceV[0] + bounceN[1]*bounceV[1] + bounceN[2]*bounceV[2];
        if (bNdotV < 0.f) bounceN = -bounceN;

        bounceOrigin = GfVec3f(
            bounceOrigin[0] + bOffset[0] + bounceDir[0] * bounceHit.t,
            bounceOrigin[1] + bOffset[1] + bounceDir[1] * bounceHit.t,
            bounceOrigin[2] + bOffset[2] + bounceDir[2] * bounceHit.t);

        // Direct lighting at bounce hit (with backface shadow filtering)
        float bounceRadiance = 0.f;
        if (scene.HasLights()) {
            for (const SpectralLight& light : scene.GetLights()) {
                GfVec3f L = light.DirectionFrom(bounceOrigin);

                bool inShadow = false;
                GfVec3f sOrig = bounceOrigin + bounceN * 0.01f;
                GfRay shadowRay;
                shadowRay.SetEnds(GfVec3d(sOrig),
                    GfVec3d(sOrig) + GfVec3d(L) * 10000.0);
                SpectralBVH::Hit shadowHit = bvh.Intersect(shadowRay, rayTime);

                if (shadowHit.valid()) {
                    GfVec3f shadowN = shadowHit.tri->faceNormal;
                    float hitFacing = shadowN[0]*L[0] + shadowN[1]*L[1] + shadowN[2]*L[2];
                    if (hitFacing < 0.f) {
                        if (light.type == SpectralLight::Type::Distant ||
                            light.type == SpectralLight::Type::Dome) {
                            inShadow = true;
                        } else {
                            float lightDist = (light.position - bounceOrigin).GetLength();
                            inShadow = (shadowHit.t < lightDist);
                        }
                    }
                }

                if (!inShadow) {
                    float bsdf = SpectralBSDF::Evaluate(*bounceMat, bounceN, bounceV, L, lambda);
                    float lightRad = light.SpectralEmission(lambda);
                    float atten = light.Attenuation(bounceOrigin);
                    bounceRadiance += bsdf * lightRad * atten;
                }
            }
            auto skyFn = [](const GfVec3f& dir, float lam) -> float {
                return _SkySpectral(dir, lam);
            };
            bounceRadiance += SpectralBSDF::EvaluateSkylighting(
                *bounceMat, bounceN, bounceV, lambda, skyFn) * 0.05f;
        } else {
            auto skyFn = [](const GfVec3f& dir, float lam) -> float {
                return _SkySpectral(dir, lam);
            };
            bounceRadiance = SpectralBSDF::EvaluateSkylighting(
                *bounceMat, bounceN, bounceV, lambda, skyFn);
        }

        bounceRadiance += bounceMat->SpectralEmission(lambda);
        radiance += pathThroughput * bounceRadiance;
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
    int                   maxBounces)
{
    SpectralGPU* gpu = _GetGPU();
    if (!gpu) {
        fprintf(stderr, "SpectralIntegrator: GPU unavailable, using CPU\n");
        RenderFrame(scene, camera, pixels, spp, depthOut, maxBounces);
        return;
    }

    if (!gpu->BuildAccel(scene)) {
        fprintf(stderr, "SpectralIntegrator: GPU accel build failed, using CPU\n");
        RenderFrame(scene, camera, pixels, spp, depthOut, maxBounces);
        return;
    }

    if (!gpu->Render(camera, camera.imageWidth, camera.imageHeight,
                     pixels, depthOut, spp, maxBounces)) {
        fprintf(stderr, "SpectralIntegrator: GPU render failed, using CPU\n");
        RenderFrame(scene, camera, pixels, spp, depthOut, maxBounces);
        return;
    }

    fprintf(stderr, "SpectralIntegrator: GPU render complete (%dx%d)\n",
            camera.imageWidth, camera.imageHeight);
}

#endif // SPECTRAL_HAS_OPTIX

PXR_NAMESPACE_CLOSE_SCOPE
