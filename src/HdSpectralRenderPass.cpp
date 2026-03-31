#include "HdSpectralRenderPass.h"
#include "HdSpectralRenderBuffer.h"

#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range2f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>

#include <algorithm>
#include <cmath>
#include <limits>

// Use all CPU cores for the pixel loop
#include <execution>    // C++17 parallel execution (MSVC supports this natively)
#include <numeric>      // std::iota
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HdSpectralRenderPass::HdSpectralRenderPass(
    HdRenderIndex*           index,
    HdRprimCollection const& collection,
    SpectralScene*           scene)
    : HdRenderPass(index, collection)
    , _scene(scene)
{
}

HdSpectralRenderPass::~HdSpectralRenderPass() = default;

// ---------------------------------------------------------------------------
// _Execute  — one frame
// ---------------------------------------------------------------------------

void
HdSpectralRenderPass::_Execute(
    HdRenderPassStateSharedPtr const& renderPassState,
    TfTokenVector const&              /*renderTags*/)
{
    _converged = false;

    // ------------------------------------------------------------------
    // 1. Resolve render buffer
    // ------------------------------------------------------------------
    HdSpectralRenderBuffer* beauty = _GetBeautyBuffer(renderPassState);
    if (!beauty) {
        TF_WARN("HdSpectral: no beauty render buffer bound — nothing to render");
        _converged = true;
        return;
    }

    const unsigned int width  = beauty->GetWidth();
    const unsigned int height = beauty->GetHeight();

    if (width == 0 || height == 0) {
        _converged = true;
        return;
    }

    // ------------------------------------------------------------------
    // 2. Camera matrices
    //
    //    HdCamera gives us:
    //      GetTransform()   — world-to-camera (view matrix)
    //      GetProjectionMatrix() — view-to-clip
    //
    //    We need:
    //      viewToWorld  = GetTransform().GetInverse()  (or GetCamera().GetTransform())
    //      projInverse  = GetProjectionMatrix().GetInverse()
    // ------------------------------------------------------------------
    const HdCamera* camera = renderPassState->GetCamera();
    if (!camera) {
        TF_WARN("HdSpectral: no camera in render pass state");
        _converged = true;
        return;
    }

    // In Hydra the camera's GetTransform() is world→camera (view matrix).
    // Its inverse is camera→world (the camera's position + orientation).
    GfMatrix4d worldToCamera = camera->GetTransform();
    GfMatrix4d viewToWorld   = worldToCamera.GetInverse();

    // Projection: clip→view inverse
    GfMatrix4d projMatrix = renderPassState->GetProjectionMatrix();
    GfMatrix4d projInverse = projMatrix.GetInverse();

    // ------------------------------------------------------------------
    // 3. Snapshot the scene geometry
    //    (single-threaded sync during Hydra 1.x Execute is guaranteed, so
    //     we can safely read the mesh map without locking here)
    // ------------------------------------------------------------------
    const auto& meshes = _scene->GetMeshes();

    // Flatten all visible triangles into one contiguous array for cache-
    // friendly access in the parallel pixel loop.
    std::vector<const SpectralTriangle*> allTris;
    allTris.reserve(_scene->TotalTriangles());
    for (auto& kv : meshes) {
        if (!kv.second.visible) continue;
        for (auto& t : kv.second.triangles) {
            allTris.push_back(&t);
        }
    }

    const size_t numTris = allTris.size();

    // ------------------------------------------------------------------
    // 4. Parallel pixel loop — each pixel is independent
    //
    //    We use std::for_each with std::execution::par_unseq so MSVC/TBB
    //    automatically distributes work across CPU cores.
    //    Replace with std::execution::seq for debugging.
    // ------------------------------------------------------------------

    // Build a row index list so we can parallelise over rows
    std::vector<unsigned int> rows(height);
    std::iota(rows.begin(), rows.end(), 0u);

    std::for_each(
        std::execution::par_unseq,
        rows.begin(), rows.end(),
        [&](unsigned int py) {
            for (unsigned int px = 0; px < width; ++px) {
                // --------------------------------------------------------
                // 4a. Generate camera ray
                // --------------------------------------------------------
                GfRay ray = _MakeCameraRay(
                    viewToWorld, projInverse, px, py, width, height);

                // --------------------------------------------------------
                // 4b. Intersect all triangles (brute force, O(n) per ray)
                //     Phase 2: replace with Embree BVH traversal
                // --------------------------------------------------------
                double          tMin = std::numeric_limits<double>::infinity();
                double          uHit = 0.0, vHit = 0.0;
                const SpectralTriangle* hitTri = nullptr;

                for (size_t ti = 0; ti < numTris; ++ti) {
                    double t, u, v;
                    if (_IntersectTriangle(ray, *allTris[ti], t, u, v)) {
                        if (t > 1e-4 && t < tMin) {
                            tMin   = t;
                            uHit   = u;
                            vHit   = v;
                            hitTri = allTris[ti];
                        }
                    }
                }

                // --------------------------------------------------------
                // 4c. Shade
                // --------------------------------------------------------
                GfVec3f color;
                if (hitTri) {
                    color = _ShadeSmoothNormal(*hitTri, uHit, vHit);
                } else {
                    // Miss — evaluate background gradient
                    GfVec3f dir = GfVec3f(ray.GetDirection());
                    float len = dir.GetLength();
                    if (len > 1e-6f) dir /= len;
                    color = _SkyColor(dir);
                }

                // --------------------------------------------------------
                // 4d. Write to render buffer
                // --------------------------------------------------------
                beauty->WritePixel(px, py, color[0], color[1], color[2], 1.0f);
            }
        }
    );

    _converged = true;
}

// ---------------------------------------------------------------------------
// _MakeCameraRay
//
//   Generates a world-space ray through the centre of pixel (px, py).
//   Converts pixel coordinates to NDC → view space via projInverse →
//   world space via viewToWorld.
// ---------------------------------------------------------------------------
GfRay
HdSpectralRenderPass::_MakeCameraRay(
    const GfMatrix4d& viewToWorld,
    const GfMatrix4d& projInverse,
    unsigned int px, unsigned int py,
    unsigned int width, unsigned int height) const
{
    // NDC: x in [-1, 1], y in [-1, 1]  (OpenGL convention, y+ up)
    // Pixel (0,0) is top-left in image coords → flip y for NDC
    const double ndcX =  2.0 * (px + 0.5) / width  - 1.0;
    const double ndcY = -2.0 * (py + 0.5) / height + 1.0;

    // Near-plane point in NDC (z = -1 for OpenGL clip space)
    GfVec4d nearNDC(ndcX, ndcY, -1.0, 1.0);
    GfVec4d farNDC (ndcX, ndcY,  1.0, 1.0);

    // Unproject to view space
    GfVec4d nearView = projInverse * nearNDC;
    GfVec4d farView  = projInverse * farNDC;

    // Perspective divide
    GfVec3d nearViewPos( nearView[0] / nearView[3],
                          nearView[1] / nearView[3],
                          nearView[2] / nearView[3]);
    GfVec3d farViewPos ( farView[0]  / farView[3],
                          farView[1]  / farView[3],
                          farView[2]  / farView[3]);

    // Transform to world space
    GfVec3d origin = viewToWorld.Transform(nearViewPos);
    GfVec3d dir    = viewToWorld.Transform(farViewPos) - origin;

    GfRay ray;
    ray.SetEnds(origin, origin + dir);
    return ray;
}

// ---------------------------------------------------------------------------
// _IntersectTriangle  — Möller–Trumbore algorithm
//
//   Classic O(1) ray–triangle intersection.  No pre-computed structure.
//   Returns true on a valid front-face hit.  Sets t, u, v (barycentric).
//   Phase 2: this entire function is replaced by Embree's rtcOccluded1 /
//            rtcIntersect1 calls — identical interface, 30–100× faster.
// ---------------------------------------------------------------------------
bool
HdSpectralRenderPass::_IntersectTriangle(
    const GfRay&           ray,
    const SpectralTriangle& tri,
    double& t_out,
    double& u_out,
    double& v_out)
{
    constexpr double kEps = 1e-8;

    const GfVec3d orig = ray.GetStartPoint();
    const GfVec3d dir  = ray.GetDirection();

    const GfVec3d v0(tri.v0[0], tri.v0[1], tri.v0[2]);
    const GfVec3d v1(tri.v1[0], tri.v1[1], tri.v1[2]);
    const GfVec3d v2(tri.v2[0], tri.v2[1], tri.v2[2]);

    GfVec3d e1 = v1 - v0;
    GfVec3d e2 = v2 - v0;

    GfVec3d h = GfCross(dir, e2);
    double  a = GfDot(e1, h);

    // Parallel check  (also culls back faces — remove abs() for two-sided)
    if (std::abs(a) < kEps) return false;

    double  f = 1.0 / a;
    GfVec3d s = orig - v0;
    double  u = f * GfDot(s, h);
    if (u < 0.0 || u > 1.0) return false;

    GfVec3d q = GfCross(s, e1);
    double  v = f * GfDot(dir, q);
    if (v < 0.0 || u + v > 1.0) return false;

    double t = f * GfDot(e2, q);
    if (t < kEps) return false;  // intersection behind ray origin

    t_out = t;
    u_out = u;
    v_out = v;
    return true;
}

// ---------------------------------------------------------------------------
// _ShadeSmoothNormal
//   Interpolates vertex normals using barycentric coords (u, v)
//   and remaps from [-1,1] to [0,1] for display.
//   w = 1 - u - v  is the weight for v0.
// ---------------------------------------------------------------------------
GfVec3f
HdSpectralRenderPass::_ShadeSmoothNormal(
    const SpectralTriangle& tri,
    double u, double v)
{
    float w = static_cast<float>(1.0 - u - v);
    float uf = static_cast<float>(u);
    float vf = static_cast<float>(v);

    GfVec3f n = tri.n0 * w + tri.n1 * uf + tri.n2 * vf;
    float len = n.GetLength();
    if (len > 1e-6f) n /= len;

    // Remap normal [-1,1] → colour [0,1]
    return GfVec3f(
        n[0] * 0.5f + 0.5f,
        n[1] * 0.5f + 0.5f,
        n[2] * 0.5f + 0.5f);
}

// ---------------------------------------------------------------------------
// _SkyColor
//   Simple hemisphere gradient: horizon is warm grey, zenith is cool blue.
//   This matches most offline renderer default backgrounds and makes it
//   obvious at a glance that the renderer is working when geometry is absent.
// ---------------------------------------------------------------------------
GfVec3f
HdSpectralRenderPass::_SkyColor(const GfVec3f& dir)
{
    // dir.y == 1 → zenith (blue), dir.y == -1 → nadir (dark)
    float t = dir[1] * 0.5f + 0.5f;   // [0,1]
    t = std::max(0.0f, std::min(1.0f, t));

    // Horizon colour
    const GfVec3f horizon(0.72f, 0.70f, 0.68f);
    // Zenith colour
    const GfVec3f zenith (0.35f, 0.55f, 0.82f);

    return GfVec3f(
        horizon[0] + (zenith[0] - horizon[0]) * t,
        horizon[1] + (zenith[1] - horizon[1]) * t,
        horizon[2] + (zenith[2] - horizon[2]) * t);
}

// ---------------------------------------------------------------------------
// _GetBeautyBuffer
//   Walk the AOV bindings in render pass state to find the "color" buffer.
// ---------------------------------------------------------------------------
HdSpectralRenderBuffer*
HdSpectralRenderPass::_GetBeautyBuffer(
    const HdRenderPassStateSharedPtr& state) const
{
    const HdRenderPassAovBindingVector& aovBindings = state->GetAovBindings();

    for (const HdRenderPassAovBinding& binding : aovBindings) {
        // We handle the "color" (beauty) AOV for now.
        // HdAovTokens->color == TfToken("color")
        if (binding.aovName == HdAovTokens->color) {
            return dynamic_cast<HdSpectralRenderBuffer*>(binding.renderBuffer);
        }
    }

    // If no explicit AOV binding, try the legacy framebuffer path
    // (Nuke may use this for simple viewer renders)
    return nullptr;
}

PXR_NAMESPACE_CLOSE_SCOPE
