#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>

#include <vector>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// SpectralTriangle
//   Flat representation of one triangle after mesh Sync + subdivision.
//   Stored in world space — transform is baked in during Sync so the
//   render pass never has to touch matrices per-triangle.
// ---------------------------------------------------------------------------
struct SpectralTriangle {
    GfVec3f v0, v1, v2;       // world-space vertices
    GfVec3f n0, n1, n2;       // per-vertex world-space normals (for smooth shading)
    GfVec3f faceNormal;        // geometric (flat) normal
};

// ---------------------------------------------------------------------------
// SpectralMeshData
//   One entry per HdSpectralMesh rprim.  The mesh writes this during Sync;
//   the render pass reads it (read-only during Execute).
// ---------------------------------------------------------------------------
struct SpectralMeshData {
    SdfPath                       id;
    std::vector<SpectralTriangle> triangles;
    bool                          visible = true;
};

// ---------------------------------------------------------------------------
// SpectralScene
//   Owned by HdSpectralRenderDelegate.  Shared (by raw pointer) with
//   HdSpectralRenderPass so both sides can access the triangle soup.
//
//   Thread safety: meshes are written during HdEngine::Execute → Sync phase
//   (single-threaded in Hydra 1.x), then read during _Execute (also
//   single-threaded for now).  The mutex is here for future safety when we
//   move to a progressive render thread in Phase 3.
// ---------------------------------------------------------------------------
class SpectralScene {
public:
    // Called by HdSpectralMesh::Sync to register/update geometry
    void SetMeshData(const SdfPath& id, SpectralMeshData&& data) {
        std::lock_guard<std::mutex> lock(_mutex);
        _meshes[id] = std::move(data);
    }

    // Called by HdSpectralMesh when the rprim is destroyed
    void RemoveMesh(const SdfPath& id) {
        std::lock_guard<std::mutex> lock(_mutex);
        _meshes.erase(id);
    }

    // Read-only access for the render pass — call inside _Execute
    const std::unordered_map<SdfPath, SpectralMeshData, SdfPath::Hash>&
    GetMeshes() const { return _meshes; }

    // Number of triangles in the entire scene (diagnostic)
    size_t TotalTriangles() const {
        size_t n = 0;
        for (auto& kv : _meshes) n += kv.second.triangles.size();
        return n;
    }

private:
    mutable std::mutex _mutex;
    std::unordered_map<SdfPath, SpectralMeshData, SdfPath::Hash> _meshes;
};

PXR_NAMESPACE_CLOSE_SCOPE
