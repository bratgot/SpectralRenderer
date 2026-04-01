#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>

#include "SpectralMaterial.h"

#include <vector>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// SpectralTriangle
// ---------------------------------------------------------------------------
struct SpectralTriangle {
    GfVec3f v0, v1, v2;       // world-space vertices
    GfVec3f n0, n1, n2;       // per-vertex world-space normals
    GfVec3f faceNormal;        // geometric (flat) normal
    SpectralMaterialId materialId = kDefaultMaterialId;
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
// ---------------------------------------------------------------------------
class SpectralScene {
public:
    SpectralScene() {
        // Material 0 is always the default white material
        _materials.push_back(SpectralMaterial::Default());
    }

    // ---- Geometry ----
    void SetMeshData(const SdfPath& id, SpectralMeshData&& data) {
        std::lock_guard<std::mutex> lock(_mutex);
        _meshes[id] = std::move(data);
    }

    void RemoveMesh(const SdfPath& id) {
        std::lock_guard<std::mutex> lock(_mutex);
        _meshes.erase(id);
    }

    const std::unordered_map<SdfPath, SpectralMeshData, SdfPath::Hash>&
    GetMeshes() const { return _meshes; }

    size_t TotalTriangles() const {
        size_t n = 0;
        for (auto& kv : _meshes) n += kv.second.triangles.size();
        return n;
    }

    // ---- Materials ----

    /// Add a material and return its ID. If a material with the same name
    /// already exists, return the existing ID.
    SpectralMaterialId AddMaterial(const SpectralMaterial& mat) {
        std::lock_guard<std::mutex> lock(_mutex);
        // Check for existing
        for (size_t i = 0; i < _materials.size(); ++i) {
            if (_materials[i].name == mat.name)
                return static_cast<SpectralMaterialId>(i);
        }
        _materials.push_back(mat);
        return static_cast<SpectralMaterialId>(_materials.size() - 1);
    }

    /// Get material by ID. Returns default material for invalid IDs.
    const SpectralMaterial& GetMaterial(SpectralMaterialId id) const {
        if (id >= 0 && id < static_cast<SpectralMaterialId>(_materials.size()))
            return _materials[id];
        return _materials[0];  // default
    }

    /// Number of materials (including default)
    size_t MaterialCount() const { return _materials.size(); }

    /// All materials
    const std::vector<SpectralMaterial>& GetMaterials() const { return _materials; }

private:
    mutable std::mutex _mutex;
    std::unordered_map<SdfPath, SpectralMeshData, SdfPath::Hash> _meshes;
    std::vector<SpectralMaterial> _materials;
};

PXR_NAMESPACE_CLOSE_SCOPE
