#pragma once

#include "HdSpectralApi.h"
#include "SpectralScene.h"

#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/ray.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdSpectralRenderBuffer;

// ---------------------------------------------------------------------------
// HdSpectralRenderPass
//
//   Called by HdEngine::Execute() to produce one frame.
//
//   Phase 1 integrator:
//     For every pixel (u, v) we:
//       1. Generate a pinhole camera ray from the HdCamera matrices.
//       2. Test the ray against every triangle in SpectralScene
//          (brute-force Möller–Trumbore — no BVH yet).
//       3. At the closest hit, evaluate smooth normals via barycentric
//          interpolation and output world-space normals remapped to [0,1]
//          as the "beauty" buffer colour.
//       4. Shade background (misses) with a simple gradient sky.
//
//   This is deliberately simple — the goal is to validate the full pipeline
//   (USD stage → Hydra sync → our render pass → Nuke viewer) before adding
//   spectral math and a BVH in Phase 2.
// ---------------------------------------------------------------------------
class HDSPECTRAL_API HdSpectralRenderPass final : public HdRenderPass
{
public:
    HdSpectralRenderPass(HdRenderIndex*           index,
                         HdRprimCollection const& collection,
                         SpectralScene*           scene);
    ~HdSpectralRenderPass() override;

protected:
    // HdRenderPass interface — called once per frame by HdEngine::Execute()
    void _Execute(HdRenderPassStateSharedPtr const& renderPassState,
                  TfTokenVector const&              renderTags) override;

    bool IsConverged() const override { return _converged; }

private:
    // -----------------------------------------------------------------------
    // Ray generation
    // -----------------------------------------------------------------------

    /// Build a world-space ray through pixel (px, py) from a pinhole camera.
    /// @param viewToWorld   Camera transform (view→world)
    /// @param projMatrix    NDC projection matrix (clip→view)
    /// @param px, py        Pixel coordinates (0,0 = top-left)
    /// @param width, height Image dimensions
    GfRay _MakeCameraRay(const GfMatrix4d& viewToWorld,
                          const GfMatrix4d& projInverse,
                          unsigned int px, unsigned int py,
                          unsigned int width, unsigned int height) const;

    // -----------------------------------------------------------------------
    // Möller–Trumbore ray–triangle intersection
    // -----------------------------------------------------------------------

    /// Returns true if the ray hits the triangle, sets t_out and uv_out.
    /// @param t_out     Distance along ray to hit point
    /// @param u_out     Barycentric coordinate for v1
    /// @param v_out     Barycentric coordinate for v2
    static bool _IntersectTriangle(const GfRay&           ray,
                                    const SpectralTriangle& tri,
                                    double& t_out,
                                    double& u_out,
                                    double& v_out);

    // -----------------------------------------------------------------------
    // Shading
    // -----------------------------------------------------------------------

    /// Interpolate smooth normal at barycentric (u, v) and map to [0,1] RGB.
    static GfVec3f _ShadeSmoothNormal(const SpectralTriangle& tri,
                                       double u, double v);

    /// Background gradient for rays that miss all geometry.
    static GfVec3f _SkyColor(const GfVec3f& dir);

    // -----------------------------------------------------------------------
    // AOV buffer helpers
    // -----------------------------------------------------------------------

    /// Resolve the "color" AOV binding to our HdSpectralRenderBuffer, or null.
    HdSpectralRenderBuffer* _GetBeautyBuffer(
        const HdRenderPassStateSharedPtr& state) const;

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    SpectralScene* _scene;      // not owned — lifetime is the delegate
    bool           _converged = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
