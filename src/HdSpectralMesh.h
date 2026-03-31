#pragma once

#include "HdSpectralApi.h"
#include "SpectralScene.h"

#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/vertexAdjacency.h>
#include <pxr/base/gf/matrix4d.h>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// HdSpectralMesh
//
//   Rprim for triangle and polygon meshes.
//
//   Sync() is called by Hydra when any dirty bits are set on this prim.
//   We:
//     1. Pull points, topology, normals and the local-to-world transform
//        from the scene delegate.
//     2. Triangulate the faces (Hydra's HdMeshUtil does this for us).
//     3. Compute smooth normals if none are authored.
//     4. Bake the transform into world-space vertex positions/normals.
//     5. Write the resulting SpectralMeshData into the SpectralScene.
//
//   Subdivision (Phase 2):
//     If subdivisionScheme != "none", we'll call OpenSubdiv's
//     Far::TopologyRefiner here before baking world space.
//     The slot is marked below with a TODO.
// ---------------------------------------------------------------------------
class HDSPECTRAL_API HdSpectralMesh final : public HdMesh
{
public:
    /// @param id        SdfPath of this rprim in the scene graph
    /// @param scene     Shared scene — we write our triangles here on Sync
    HdSpectralMesh(SdfPath const& id, SpectralScene* scene);
    ~HdSpectralMesh() override;

    // -----------------------------------------------------------------------
    // HdRprim interface
    // -----------------------------------------------------------------------

    /// Return the set of dirty bits Hydra should track for us.
    /// Over-declaring is safe; under-declaring misses updates.
    HdDirtyBits GetInitialDirtyBitsMask() const override;

    /// Called by Hydra every frame this prim has dirty bits set.
    void Sync(HdSceneDelegate* sceneDelegate,
              HdRenderParam*   renderParam,
              HdDirtyBits*     dirtyBits,
              TfToken const&   reprToken) override;

protected:
    // -----------------------------------------------------------------------
    // HdMesh interface
    // -----------------------------------------------------------------------

    /// Which dirty bits should propagate from this prim to its children.
    /// Return AllDirty to be safe in Phase 1.
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

    /// Populate repr caches — we don't use Hydra reprs, so this is a no-op.
    void _InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits) override;

private:
    // Rebuild the triangle soup from current scene data and write to _scene
    void _RebuildTriangles(
        HdSceneDelegate*       sceneDelegate,
        const VtVec3fArray&    points,
        const VtIntArray&      triangleIndices,   // flat: [v0a,v1a,v2a, v0b,...] 
        const VtVec3fArray&    normals,           // per-vertex or empty
        bool                   normalsIsVertex,   // true=per-vertex, false=per-face
        const GfMatrix4d&      localToWorld);

    SpectralScene* _scene;   // raw pointer, lifetime owned by delegate
};

PXR_NAMESPACE_CLOSE_SCOPE
