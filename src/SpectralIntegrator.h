#pragma once

// ---------------------------------------------------------------------------
// SpectralIntegrator
//
//   Self-contained CPU ray tracer shared between:
//     - HdSpectralRenderPass  (Hydra viewer path)
//     - SpectralRenderIop     (NDK node graph / farm path)
//
//   Phase 1: Möller–Trumbore brute force + smooth normal shading.
//   Phase 2: Embree BVH replaces _IntersectScene().
//   Phase 3: Hero wavelength spectral sampling replaces _Shade().
//
//   Thread safety: RenderTile() is stateless given the input data — safe
//   to call from multiple threads simultaneously (used by the RenderIop
//   tile scheduler and by std::execution::par_unseq in the render pass).
// ---------------------------------------------------------------------------

#include "SpectralScene.h"
#include "SpectralSpectrum.h"
#include "SpectralBSDF.h"

#ifdef SPECTRAL_HAS_EMBREE
#include "SpectralBVH.h"
#endif

#ifdef SPECTRAL_HAS_OPTIX
#include "SpectralGPU.h"
#endif

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/ray.h>

#include <vector>
#include <functional>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// SpectralCamera
//   Minimal camera representation passed into the integrator.
//   Both the render pass (from HdCamera) and the RenderIop (from
//   DDImage::CameraOp) fill this struct before calling RenderTile().
// ---------------------------------------------------------------------------
struct SpectralCamera {
    GfMatrix4d viewToWorld;   // camera→world  (inverse of view matrix)
    GfMatrix4d projInverse;   // clip→view     (inverse of projection matrix)
    unsigned int imageWidth  = 1920;
    unsigned int imageHeight = 1080;
    double pixelAspect       = 1.0;   // pixel aspect ratio from format
    float  shutterOpen       = 0.f;   // motion blur shutter [0,1] for Embree
    float  shutterClose      = 0.f;   // 0,0 = no motion blur
    float  adaptiveThreshold = 0.05f; // 0 = disabled, 0.05 = default
    bool   blueNoise         = true;  // R2 quasi-random sampling
};

// ---------------------------------------------------------------------------
// SpectralIntegrator
// ---------------------------------------------------------------------------
class SpectralIntegrator {
public:
    // -----------------------------------------------------------------------
    // RenderTile
    //
    //   Renders a rectangular tile of pixels into `pixels` (pre-allocated,
    //   RGBA float32, row-major, width = tileW).
    //
    //   @param scene      Read-only scene geometry
    //   @param camera     Camera matrices + image dimensions
    //   @param tileX      Tile left edge in image space (pixels)
    //   @param tileY      Tile top edge in image space (pixels)
    //   @param tileW      Tile width in pixels
    //   @param tileH      Tile height in pixels
    //   @param pixels     Output buffer: tileW * tileH * 4 floats (RGBA)
    //   @param spp        Samples per pixel (Phase 1: 1, Phase 3: N)
    // -----------------------------------------------------------------------
    static void RenderTile(
        const SpectralScene& scene,
        const SpectralCamera& camera,
        unsigned int tileX,
        unsigned int tileY,
        unsigned int tileW,
        unsigned int tileH,
        float* pixels,
        int spp = 1);

    // -----------------------------------------------------------------------
    // RenderFrame
    //
    //   Convenience wrapper — renders the full image into a flat buffer.
    //   Uses std::execution::par_unseq over rows for CPU parallelism.
    //   `pixels` must be pre-allocated: width * height * 4 floats.
    // -----------------------------------------------------------------------
    static void RenderFrame(
        const SpectralScene& scene,
        const SpectralCamera& camera,
        float* pixels,
        int spp = 1,
        float* depthOut = nullptr,
        int maxBounces = 4,
        float* objectIdOut = nullptr,
        float* materialIdOut = nullptr);

#ifdef SPECTRAL_HAS_OPTIX
    /// GPU render path using OptiX.
    /// Falls back to CPU if GPU init fails.
    static void RenderFrameGPU(
        const SpectralScene& scene,
        const SpectralCamera& camera,
        float* pixels,
        int spp = 1,
        float* depthOut = nullptr,
        int maxBounces = 4);

    /// Check if GPU rendering is available.
    static bool IsGPUAvailable();

    /// Denoise a framebuffer using OptiX AI denoiser (GPU only).
    static void DenoiseGPU(unsigned int width, unsigned int height, float* pixels);
#endif  // optional per-pixel depth output

private:
    // Ray generation
    static GfRay _MakeRay(const SpectralCamera& cam,
                           unsigned int px, unsigned int py,
                           float jitterX = 0.5f, float jitterY = 0.5f);

    // Intersection — returns true on hit, fills t/u/v/tri
    struct Hit {
        double t = std::numeric_limits<double>::infinity();
        double u = 0, v = 0;
        const SpectralTriangle* tri = nullptr;
        bool valid() const { return tri != nullptr; }
    };

    static Hit _IntersectScene(const GfRay& ray,
                                const std::vector<const SpectralTriangle*>& tris);

    static bool _IntersectTriangle(const GfRay& ray,
                                    const SpectralTriangle& tri,
                                    double& t, double& u, double& v);

    // Shading
    static GfVec3f _ShadeSmoothNormal(const SpectralTriangle& tri,
                                       double u, double v);
    static GfVec3f _SkyColor(const GfVec3f& dir);

    // Spectral shading — returns spectral radiance at a single wavelength
    static float _ShadeSpectral(const SpectralTriangle& tri,
                                 double u, double v, float lambda,
                                 const SpectralMaterial& mat,
                                 const SpectralScene& scene,
                                 const GfVec3f& hitPos,
                                 const GfVec3f& rayDir,
                                 int maxBounces,
                                 unsigned int& rngSeed,
                                 const SpectralBVH& bvh,
                                 float rayTime = 0.f);
    static float _SkySpectral(const GfVec3f& dir, float lambda);

    // Simple hash-based RNG for per-pixel, per-sample jitter
    static float _Hash(unsigned int seed);
};

PXR_NAMESPACE_CLOSE_SCOPE
