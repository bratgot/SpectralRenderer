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
#include "SpectralVolume.h"

#ifdef SPECTRAL_HAS_EMBREE
#include "SpectralBVH.h"
#include "SpectralPhotonMap.h"
#endif

#ifdef SPECTRAL_HAS_OPTIX
#include "SpectralGPU.h"
#endif

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/ray.h>
#include <unordered_set>

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
    // Camera motion blur: end-of-shutter camera position
    GfMatrix4d viewToWorldClose; // camera→world at shutter close
    bool       cameraMblur   = false;
    unsigned int imageWidth  = 1920;
    unsigned int imageHeight = 1080;
    double pixelAspect       = 1.0;   // pixel aspect ratio from format
    float  shutterOpen       = 0.f;   // motion blur shutter [0,1] for Embree
    float  shutterClose      = 0.f;   // 0,0 = no motion blur
    float  adaptiveThreshold = 0.05f; // 0 = disabled, 0.05 = default
    bool   blueNoise         = true;  // R2 quasi-random sampling
    bool   scanlineCompat    = true;  // Direct RGB shading (no spectral XYZ)
    bool   neutralBalance    = true;  // Correct spectral white balance shift
    int    projectionMode    = 0;     // 0=perspective, 1=UV, 2=spherical
    int    edgeSamples       = 0;     // Edge AA supersamples (0=disabled)

    // Wireframe overlay
    bool   wireframeEnable   = false;
    float  wireThickness     = 1.f;
    float  wireOpacity       = 1.f;
    float  wireColor[3]      = {0.f, 0.f, 0.f};
    bool   wireDashed        = false;
    float  wireDashLength    = 8.f;
    float  wireGapLength     = 4.f;
    int    wireNth           = 1;
    int    wireStyle         = 0;  // 0=solid, 1=guide, 2=architectural, 3=hidden-line

    // Shadow catcher material IDs
    std::unordered_set<int> shadowCatcherMatIds;

    // UV projection lookup (CPU-rasterized, passed to GPU)
    const int*   uvTriIndex  = nullptr;
    const float* uvBaryU     = nullptr;
    const float* uvBaryV     = nullptr;
    size_t       uvBufSize   = 0;   // W*H pixels
    int    aoSamples         = 8;    // ambient occlusion samples (0 = disabled)
    float  aoRadius          = 5.f;  // AO max ray distance
    int    refractionBounces = 8;    // separate limit for glass paths
    float  focalLength       = 50.f; // mm
    float  fStop             = 0.f;  // 0 = pinhole (no DOF)
    float  focusDistance     = 100.f; // world units
    int    volumeSpp         = 0;    // 0 = use main spp, else separate vol samples
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
    // AOV buffer struct for production passes
    struct AOVBuffers {
        float* normal   = nullptr;  // 3 floats per pixel (Nx,Ny,Nz)
        float* position = nullptr;  // 3 floats per pixel (Px,Py,Pz)
        float* uv       = nullptr;  // 2 floats per pixel (u,v)
        float* albedo   = nullptr;  // 3 floats per pixel (r,g,b)
        float* direct   = nullptr;  // 3 floats per pixel
        float* indirect = nullptr;  // 3 floats per pixel
        float* emission = nullptr;  // 3 floats per pixel
        float* pRef     = nullptr;  // 3 floats per pixel (undisplaced position)
        // LPE decomposition
        float* diffuseDirect   = nullptr;
        float* specularDirect  = nullptr;
        float* diffuseIndirect = nullptr;
        float* specularIndirect = nullptr;
        float* transmission    = nullptr;
    };

    static void RenderFrame(
        const SpectralScene& scene,
        const SpectralCamera& camera,
        float* pixels,
        int spp = 1,
        float* depthOut = nullptr,
        int maxBounces = 4,
        float* objectIdOut = nullptr,
        float* materialIdOut = nullptr,
        const AOVBuffers* aovs = nullptr,
        float* aoOut = nullptr,
        const SpectralPhotonMap* photonMap = nullptr,
        float gatherRadius = 0.5f,
        int colorSpace = 0,
        const SpectralVolume* const* volumes = nullptr,
        int numVolumes = 0);

#ifdef SPECTRAL_HAS_OPTIX
    /// GPU render path using OptiX.
    using StripCallback = std::function<void(int y0, int y1)>;
    static void RenderFrameGPU(
        const SpectralScene& scene,
        const SpectralCamera& camera,
        float* pixels,
        int spp = 1,
        float* depthOut = nullptr,
        int maxBounces = 4,
        int colorSpace = 0,
        const SpectralVolume* const* volumes = nullptr,
        int numVolumes = 0,
        StripCallback stripCallback = nullptr,
        int numStrips = 1);

    static bool IsGPUAvailable();
    static void DenoiseGPU(unsigned int width, unsigned int height, float* pixels);
    static void InvalidateGPUAccel();
#endif

    /// Build a photon map for spectral caustics.
    /// Traces photons from lights through specular surfaces,
    /// storing them where they hit diffuse geometry.
    /// Each photon carries a single wavelength — dispersion
    /// naturally separates the rainbow in the photon map.
    static void BuildPhotonMap(
        const SpectralScene& scene,
        SpectralPhotonMap& photonMap,
        int numPhotons = 100000,
        int maxBounces = 8);

    /// Compute ambient occlusion into a separate buffer.
    static void ComputeAO(
        const SpectralScene& scene,
        const SpectralCamera& camera,
        float* aoOut,           // W*H floats, 0=occluded, 1=open
        int    aoSamples,       // rays per pixel
        float  aoRadius);       // max ray distance

    /// Quick single-ray pass for geometry AOVs (N, P, UV, albedo, IDs, depth).
    /// Used after GPU render which doesn't fill these.
    static void ComputeGeometryAOVs(
        const SpectralScene& scene,
        const SpectralCamera& camera,
        float* normalOut,       // 3 floats per pixel
        float* posOut,          // 3 floats per pixel
        float* pRefOut,         // 3 floats per pixel (undisplaced)
        float* uvOut,           // 2 floats per pixel
        float* albedoOut,       // 3 floats per pixel
        float* objectIdOut,     // 1 float per pixel
        float* materialIdOut,   // 1 float per pixel
        float* depthOut);       // 1 float per pixel

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

    // Shading component decomposition
    struct ShadeComponents {
        float direct   = 0.f;  // direct lighting
        float indirect = 0.f;  // bounce/indirect
        float emission = 0.f;  // emissive
        // LPE decomposition
        float diffuseDirect   = 0.f;
        float specularDirect  = 0.f;
        float diffuseIndirect = 0.f;
        float specularIndirect = 0.f;
        float transmission    = 0.f;
    };

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
                                 float rayTime = 0.f,
                                 ShadeComponents* comps = nullptr,
                                 const SpectralPhotonMap* photonMap = nullptr,
                                 float gatherRadius = 0.5f,
                                 const SpectralVolume* const* volumes = nullptr,
                                 int numVolumes = 0);
    static float _SkySpectral(const GfVec3f& dir, float lambda);

    // Simple hash-based RNG for per-pixel, per-sample jitter
    static float _Hash(unsigned int seed);
};

PXR_NAMESPACE_CLOSE_SCOPE
