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
    float*                pixels)
{
#ifdef SPECTRAL_HAS_EMBREE
    // Build BVH once for the entire frame — shared read-only across threads
    SpectralBVH bvh;
    bvh.Build(scene);

    // Row indices for parallel dispatch
    const unsigned int tileW = camera.imageWidth;
    const unsigned int tileH = camera.imageHeight;

    std::vector<unsigned int> rows(tileH);
    std::iota(rows.begin(), rows.end(), 0u);

    std::for_each(
        std::execution::par_unseq,
        rows.begin(), rows.end(),
        [&](unsigned int imageY)
        {
            for (unsigned int imageX = 0; imageX < tileW; ++imageX) {
                GfRay ray = _MakeRay(camera, imageX, imageY);

                GfVec3f color;
                SpectralBVH::Hit hit = bvh.Intersect(ray);
                if (hit.valid()) {
                    color = _ShadeSmoothNormal(*hit.tri,
                                               static_cast<double>(hit.u),
                                               static_cast<double>(hit.v));
                } else {
                    GfVec3f dir = GfVec3f(ray.GetDirection());
                    float len = dir.GetLength();
                    if (len > 1e-6f) dir /= len;
                    color = _SkyColor(dir);
                }

                float* px = pixels + (imageY * tileW + imageX) * 4;
                px[0] = color[0];
                px[1] = color[1];
                px[2] = color[2];
                px[3] = 1.0f;
            }
        });

#else
    // Brute-force fallback — no Embree
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

PXR_NAMESPACE_CLOSE_SCOPE
