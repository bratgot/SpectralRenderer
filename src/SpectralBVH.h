#pragma once

// ---------------------------------------------------------------------------
// SpectralBVH
//
//   Wraps Embree 4 for O(log n) ray–triangle intersection.
//   Built from a SpectralScene snapshot; immutable once built.
//
//   Usage:
//     SpectralBVH bvh;
//     bvh.Build(scene);           // builds Embree BVH
//     auto hit = bvh.Intersect(ray);
//     if (hit.valid()) { ... }
//     // bvh destructor releases Embree resources
//
//   Thread safety: Intersect() is safe to call from multiple threads
//   simultaneously (Embree scenes are read-only after commit).
// ---------------------------------------------------------------------------

#ifdef SPECTRAL_HAS_EMBREE

#include "SpectralScene.h"

#include <embree4/rtcore.h>

#include <pxr/base/gf/ray.h>
#include <pxr/base/gf/vec3f.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralBVH
{
public:
    SpectralBVH();
    ~SpectralBVH();

    // Non-copyable, movable
    SpectralBVH(const SpectralBVH&)            = delete;
    SpectralBVH& operator=(const SpectralBVH&) = delete;
    SpectralBVH(SpectralBVH&& other) noexcept;
    SpectralBVH& operator=(SpectralBVH&& other) noexcept;

    /// Build the BVH from the current scene snapshot.
    /// All visible meshes are flattened into a single Embree geometry.
    /// Call this once per frame before rendering.
    void Build(const SpectralScene& scene);

    /// Release Embree resources. Called automatically by destructor.
    void Release();

    /// True if the BVH has been built and contains geometry.
    bool IsValid() const { return _rtcScene != nullptr && !_triangles.empty(); }

    /// Number of triangles in the BVH.
    size_t TriangleCount() const { return _triangles.size(); }

    // -----------------------------------------------------------------
    // Hit result — mirrors SpectralIntegrator::Hit but includes
    // a pointer to the triangle for shading.
    // -----------------------------------------------------------------
    struct Hit {
        float t = 1e30f;
        float u = 0.f, v = 0.f;
        const SpectralTriangle* tri = nullptr;
        bool valid() const { return tri != nullptr; }
    };

    /// Trace a single ray against the BVH. Thread-safe.
    Hit Intersect(const GfRay& ray, float time = 0.f) const;

private:
    RTCDevice _rtcDevice = nullptr;
    RTCScene  _rtcScene  = nullptr;

    // Flat triangle list matching Embree's geometry indexing.
    // Embree gives us primID on hit — we use it to index into this.
    std::vector<const SpectralTriangle*> _triangles;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_EMBREE
