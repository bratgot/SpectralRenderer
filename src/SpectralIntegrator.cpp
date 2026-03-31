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
    int                   spp)
{
#ifdef SPECTRAL_HAS_EMBREE
    SpectralBVH bvh;
    bvh.Build(scene);

    const unsigned int W = camera.imageWidth;
    const unsigned int H = camera.imageHeight;
    const bool spectral = (spp > 1);

    std::vector<unsigned int> rows(H);
    std::iota(rows.begin(), rows.end(), 0u);

    std::for_each(
        std::execution::par_unseq,
        rows.begin(), rows.end(),
        [&](unsigned int imageY)
        {
            for (unsigned int imageX = 0; imageX < W; ++imageX) {

                if (!spectral) {
                    // ---- SPP=1: fast normal-as-colour mode ----
                    GfRay ray = _MakeRay(camera, imageX, imageY);
                    SpectralBVH::Hit hit = bvh.Intersect(ray);

                    GfVec3f color;
                    if (hit.valid()) {
                        color = _ShadeSmoothNormal(
                            *hit.tri,
                            static_cast<double>(hit.u),
                            static_cast<double>(hit.v));
                    } else {
                        GfVec3f dir = GfVec3f(ray.GetDirection());
                        float len = dir.GetLength();
                        if (len > 1e-6f) dir /= len;
                        color = _SkyColor(dir);
                    }

                    float* px = pixels + (imageY * W + imageX) * 4;
                    px[0] = color[0];
                    px[1] = color[1];
                    px[2] = color[2];
                    px[3] = 1.0f;

                } else {
                    // ---- SPP>1: hero wavelength spectral mode ----
                    float X = 0.f, Y = 0.f, Z = 0.f;

                    for (int s = 0; s < spp; ++s) {
                        unsigned int seed = (imageY * W + imageX) * 1031 + s * 6571;

                        float jx = _Hash(seed);
                        float jy = _Hash(seed + 1);

                        // Stratified wavelength sampling
                        float wu = (float(s) + _Hash(seed + 2)) / float(spp);
                        float lambda = SpectralSpectrum::SampleWavelength(wu);

                        GfRay ray = _MakeRay(camera, imageX, imageY, jx, jy);
                        SpectralBVH::Hit hit = bvh.Intersect(ray);

                        float radiance;
                        if (hit.valid()) {
                            radiance = _ShadeSpectral(
                                *hit.tri,
                                static_cast<double>(hit.u),
                                static_cast<double>(hit.v),
                                lambda);
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

                    float* px = pixels + (imageY * W + imageX) * 4;
                    px[0] = std::max(0.f, rgb[0]);
                    px[1] = std::max(0.f, rgb[1]);
                    px[2] = std::max(0.f, rgb[2]);
                    px[3] = 1.0f;
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
// _ShadeSpectral — spectral radiance from a surface hit at one wavelength
//
//   White diffuse material: flat 0.8 reflectance across all wavelengths.
//   Lit by the spectral sky using a simple N·L hemisphere model.
//   The surface takes on the colour of the sky illumination —
//   blue from above, warm from the horizon — proving spectral transport.
//
//   Phase 4 will replace this with a full spectral Disney BSDF.
// ---------------------------------------------------------------------------
float SpectralIntegrator::_ShadeSpectral(
    const SpectralTriangle& tri, double u, double v, float lambda)
{
    float w  = float(1.0 - u - v);
    float uf = float(u);
    float vf = float(v);

    GfVec3f n = tri.n0 * w + tri.n1 * uf + tri.n2 * vf;
    float len = n.GetLength();
    if (len > 1e-6f) n /= len;

    // White diffuse reflectance (flat across spectrum)
    const float albedo = 0.8f;

    // Simple hemisphere lighting: blend sky radiance from above and below
    // Upper hemisphere contribution (N·up clamped)
    float nDotUp = std::max(0.f, n[1]);
    float skyAbove = _SkySpectral(GfVec3f(0.f, 1.f, 0.f), lambda);

    // Lower hemisphere: ground bounce (dim warm)
    float nDotDown = std::max(0.f, -n[1]);
    float skyBelow = _SkySpectral(GfVec3f(0.f, -1.f, 0.f), lambda) * 0.3f;

    // Side lighting (horizon)
    float nDotSide = 1.f - nDotUp - nDotDown;
    float skySide  = _SkySpectral(GfVec3f(1.f, 0.f, 0.f), lambda);

    float irradiance = nDotUp * skyAbove + nDotDown * skyBelow + nDotSide * skySide;

    return albedo * irradiance;
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

PXR_NAMESPACE_CLOSE_SCOPE
