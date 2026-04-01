#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>

#include "SpectralMaterial.h"
#include "SpectralLight.h"
#include "SpectralTexture.h"

#include <vector>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// SpectralTriangle
// ---------------------------------------------------------------------------
struct SpectralTriangle {
    GfVec3f v0, v1, v2;       // world-space vertices (shutter open / static)
    GfVec3f n0, n1, n2;       // per-vertex world-space normals
    GfVec3f faceNormal;        // geometric (flat) normal
    GfVec2f uv0, uv1, uv2;   // per-vertex texture coordinates
    SpectralMaterialId materialId = kDefaultMaterialId;
    int     objectId = 0;          // per-mesh object ID for AOV

    // Motion blur: vertex positions at shutter close
    // If hasMotion is false, these are unused (static geometry)
    GfVec3f v0_close, v1_close, v2_close;
    bool    hasMotion = false;

    // Interpolate vertices at time t in [0,1] (0=open, 1=close)
    void LerpPositions(float t, GfVec3f& p0, GfVec3f& p1, GfVec3f& p2) const {
        if (!hasMotion || t <= 0.f) { p0 = v0; p1 = v1; p2 = v2; return; }
        if (t >= 1.f) { p0 = v0_close; p1 = v1_close; p2 = v2_close; return; }
        p0 = v0 * (1.f - t) + v0_close * t;
        p1 = v1 * (1.f - t) + v1_close * t;
        p2 = v2 * (1.f - t) + v2_close * t;
    }
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
    int                           objectId = 0;   // unique per mesh
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

    /// Assign a unique object ID (call once per mesh during loading)
    int NextObjectId() { return _nextObjectId++; }

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

    // ---- Lights ----

    void AddLight(const SpectralLight& light) {
        std::lock_guard<std::mutex> lock(_mutex);
        _lights.push_back(light);
    }

    void ClearLights() {
        std::lock_guard<std::mutex> lock(_mutex);
        _lights.clear();
    }

    const std::vector<SpectralLight>& GetLights() const { return _lights; }
    size_t LightCount() const { return _lights.size(); }

    /// True if there are explicit lights in the scene.
    /// If false, the integrator falls back to sky-only lighting.
    bool HasLights() const { return !_lights.empty(); }

    // ---- Textures ----

    /// Load a texture and return its ID. If already loaded, returns existing ID.
    int LoadTexture(const std::string& filePath) {
        std::lock_guard<std::mutex> lock(_mutex);
        // Check if already loaded
        for (size_t i = 0; i < _textures.size(); ++i) {
            if (_textures[i].GetPath() == filePath)
                return static_cast<int>(i);
        }
        SpectralTexture tex;
        if (!tex.Load(filePath)) return -1;
        _textures.push_back(std::move(tex));
        return static_cast<int>(_textures.size() - 1);
    }

    /// Get texture by ID. Returns nullptr for invalid IDs.
    const SpectralTexture* GetTexture(int id) const {
        if (id >= 0 && id < static_cast<int>(_textures.size()))
            return &_textures[id];
        return nullptr;
    }

    size_t TextureCount() const { return _textures.size(); }

private:
    mutable std::mutex _mutex;
    std::unordered_map<SdfPath, SpectralMeshData, SdfPath::Hash> _meshes;
    std::vector<SpectralMaterial> _materials;
    std::vector<SpectralLight> _lights;
    std::vector<SpectralTexture> _textures;
    int _nextObjectId = 1;  // 0 = background/no hit
};

PXR_NAMESPACE_CLOSE_SCOPE
