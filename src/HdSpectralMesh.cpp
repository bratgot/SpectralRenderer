#include "HdSpectralMesh.h"

#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/smoothNormals.h>
#include <pxr/imaging/hd/vertexAdjacency.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/usd/usdGeom/tokens.h>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
HdSpectralMesh::HdSpectralMesh(SdfPath const& id, SpectralScene* scene)
    : HdMesh(id)
    , _scene(scene)
{
}

HdSpectralMesh::~HdSpectralMesh()
{
    // Remove our geometry from the scene so it won't be rendered
    if (_scene) {
        _scene->RemoveMesh(GetId());
    }
}

// ---------------------------------------------------------------------------
// Dirty bit mask
//
//   We want to be notified when any of these change:
//     • Points (vertex positions)
//     • Topology (face connectivity, subdivision scheme)
//     • Normals (either authored or computed)
//     • The local-to-world transform
//     • Visibility
//     • Primvars (for future material binding, UVs, etc.)
// ---------------------------------------------------------------------------
HdDirtyBits
HdSpectralMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::DirtyPoints
         | HdChangeTracker::DirtyTopology
         | HdChangeTracker::DirtyNormals
         | HdChangeTracker::DirtyTransform
         | HdChangeTracker::DirtyVisibility
         | HdChangeTracker::DirtyPrimvar
         | HdChangeTracker::DirtyMaterialId
         | HdChangeTracker::AllDirty;  // safe during Phase 1
}

HdDirtyBits
HdSpectralMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void
HdSpectralMesh::_InitRepr(TfToken const& /*reprToken*/, HdDirtyBits* /*dirtyBits*/)
{
    // We don't use Hydra's repr/drawing system — no-op.
}

// ---------------------------------------------------------------------------
// Sync — the main work happens here
// ---------------------------------------------------------------------------
void
HdSpectralMesh::Sync(HdSceneDelegate* sceneDelegate,
                     HdRenderParam*   /*renderParam*/,
                     HdDirtyBits*     dirtyBits,
                     TfToken const&   /*reprToken*/)
{
    const SdfPath& id = GetId();

    // ------------------------------------------------------------------
    // 1. Visibility
    // ------------------------------------------------------------------
    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
    }

    // If invisible, remove from scene and bail early
    if (!IsVisible()) {
        SpectralMeshData data;
        data.id      = id;
        data.visible = false;
        _scene->SetMeshData(id, std::move(data));
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    // ------------------------------------------------------------------
    // 2. Transform  (local-to-world)
    // ------------------------------------------------------------------
    GfMatrix4d localToWorld(1.0);
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
        localToWorld = sceneDelegate->GetTransform(id);
    }

    // ------------------------------------------------------------------
    // 3. Topology  (face counts, vertex indices, subdivision scheme)
    // ------------------------------------------------------------------
    HdMeshTopology topology;
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        topology = GetMeshTopology(sceneDelegate);
    } else {
        // Re-use cached topology even when only transform changed
        topology = GetMeshTopology(sceneDelegate);
    }

    // ------------------------------------------------------------------
    // 4. Points
    // ------------------------------------------------------------------
    VtVec3fArray points;
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        VtValue pointsVal = sceneDelegate->Get(id, HdTokens->points);
        if (pointsVal.IsHolding<VtVec3fArray>()) {
            points = pointsVal.UncheckedGet<VtVec3fArray>();
        }
    }

    if (points.empty()) {
        // Nothing to render
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    // ------------------------------------------------------------------
    // 5. Triangulation via HdMeshUtil
    //
    //    HdMeshUtil handles arbitrary polygon soups and converts them to
    //    a flat triangle index list.  It also provides the mapping from
    //    triangle index back to the original face (useful for face-varying
    //    primvars in later phases).
    // ------------------------------------------------------------------
    HdMeshUtil meshUtil(&topology, id);

    VtVec3iArray trianglesFaceVertexIndices;
    VtIntArray   primitiveParams;           // maps triangle → original face
    meshUtil.ComputeTriangleIndices(
        &trianglesFaceVertexIndices,
        &primitiveParams,
        /*quadsTriangulated=*/nullptr);

    // ------------------------------------------------------------------
    // 6. Normals
    //
    //    Priority order (matches most production renderers):
    //      a) Authored per-vertex normals in the USD prim
    //      b) Authored per-face normals
    //      c) Computed smooth normals from topology (our fallback)
    // ------------------------------------------------------------------
    VtVec3fArray normals;
    bool         normalsIsVertex = true;

    VtValue normalsVal = sceneDelegate->Get(id, HdTokens->normals);
    if (normalsVal.IsHolding<VtVec3fArray>()) {
        normals = normalsVal.UncheckedGet<VtVec3fArray>();
        // Decide interpolation: per-vertex if count matches points
        normalsIsVertex = (normals.size() == points.size());
    }

    if (normals.empty()) {
        // Compute smooth normals using Hydra's adjacency table
        Hd_VertexAdjacency adjacency;
        adjacency.BuildAdjacencyTable(&topology);
        normals = Hd_SmoothNormals::ComputeSmoothNormals(
            &adjacency,
            static_cast<int>(points.size()),
            points.cdata());
        normalsIsVertex = true;
    }

    // ------------------------------------------------------------------
    // 7. TODO (Phase 2): OpenSubdiv tessellation
    //
    //    if (topology.GetScheme() != PxOsdOpenSubdivTokens->none) {
    //        // Call Far::TopologyRefiner + stencil evaluation here.
    //        // Replace points/normals/trianglesFaceVertexIndices with
    //        // the tessellated limit surface.
    //    }
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // 8. Flatten topology to flat VtIntArray for our helper
    // ------------------------------------------------------------------
    VtIntArray flatIndices;
    flatIndices.reserve(trianglesFaceVertexIndices.size() * 3);
    for (const GfVec3i& tri : trianglesFaceVertexIndices) {
        flatIndices.push_back(tri[0]);
        flatIndices.push_back(tri[1]);
        flatIndices.push_back(tri[2]);
    }

    // ------------------------------------------------------------------
    // 9. Build world-space triangle soup and write to scene
    // ------------------------------------------------------------------
    _RebuildTriangles(sceneDelegate, points, flatIndices,
                      normals, normalsIsVertex, localToWorld);

    *dirtyBits = HdChangeTracker::Clean;
}

// ---------------------------------------------------------------------------
// _RebuildTriangles
//   Bakes local-space data into world space and writes to SpectralScene.
// ---------------------------------------------------------------------------
void
HdSpectralMesh::_RebuildTriangles(
    HdSceneDelegate*    /*sceneDelegate*/,
    const VtVec3fArray& points,
    const VtIntArray&   indices,
    const VtVec3fArray& normals,
    bool                normalsIsVertex,
    const GfMatrix4d&   localToWorld)
{
    // Normal transform = inverse-transpose of the upper-left 3×3
    // (handles non-uniform scale correctly)
    GfMatrix4d normalMatrix = localToWorld.GetInverse().GetTranspose();

    const int triCount = static_cast<int>(indices.size()) / 3;

    SpectralMeshData data;
    data.id      = GetId();
    data.visible = true;
    data.triangles.reserve(triCount);

    auto xfPoint = [&](const GfVec3f& p) -> GfVec3f {
        return GfVec3f(localToWorld.Transform(GfVec3d(p)));
    };
    auto xfNormal = [&](const GfVec3f& n) -> GfVec3f {
        GfVec3f xn = GfVec3f(normalMatrix.TransformDir(GfVec3d(n)));
        float len = xn.GetLength();
        return (len > 1e-6f) ? (xn / len) : GfVec3f(0.f, 1.f, 0.f);
    };

    for (int t = 0; t < triCount; ++t) {
        const int i0 = indices[t * 3 + 0];
        const int i1 = indices[t * 3 + 1];
        const int i2 = indices[t * 3 + 2];

        // Guard against out-of-range indices (malformed USD mesh)
        const int maxIdx = static_cast<int>(points.size()) - 1;
        if (i0 > maxIdx || i1 > maxIdx || i2 > maxIdx) continue;

        SpectralTriangle tri;
        tri.v0 = xfPoint(points[i0]);
        tri.v1 = xfPoint(points[i1]);
        tri.v2 = xfPoint(points[i2]);

        // Normals — handle per-vertex vs per-face interpolation
        if (normalsIsVertex && static_cast<int>(normals.size()) > maxIdx) {
            tri.n0 = xfNormal(normals[i0]);
            tri.n1 = xfNormal(normals[i1]);
            tri.n2 = xfNormal(normals[i2]);
        } else if (!normals.empty() && t < static_cast<int>(normals.size())) {
            // Per-face: same normal for all three corners
            GfVec3f fn = xfNormal(normals[t]);
            tri.n0 = tri.n1 = tri.n2 = fn;
        } else {
            // Geometric fallback
            GfVec3f e0 = tri.v1 - tri.v0;
            GfVec3f e1 = tri.v2 - tri.v0;
            GfVec3f fn = GfCross(e0, e1);
            float len = fn.GetLength();
            fn = (len > 1e-8f) ? (fn / len) : GfVec3f(0.f, 1.f, 0.f);
            tri.n0 = tri.n1 = tri.n2 = fn;
        }

        // Geometric (flat) face normal — useful for backface culling later
        {
            GfVec3f e0 = tri.v1 - tri.v0;
            GfVec3f e1 = tri.v2 - tri.v0;
            GfVec3f fn = GfCross(e0, e1);
            float len = fn.GetLength();
            tri.faceNormal = (len > 1e-8f) ? (fn / len) : GfVec3f(0.f, 1.f, 0.f);
        }

        data.triangles.push_back(tri);
    }

    _scene->SetMeshData(GetId(), std::move(data));
}

PXR_NAMESPACE_CLOSE_SCOPE
