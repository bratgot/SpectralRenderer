#pragma once

// ---------------------------------------------------------------------------
// SpectralSubdiv
//
//   Wraps OpenSubdiv Far::TopologyRefiner to tessellate Catmull-Clark
//   (and other subdivision scheme) meshes into refined triangle soups.
//
//   Usage:
//     SpectralSubdiv::Input input;
//     input.points = ...;
//     input.faceVertexCounts = ...;
//     input.faceVertexIndices = ...;
//     input.scheme = SpectralSubdiv::Scheme::CatmullClark;
//     input.level = 2;
//
//     SpectralSubdiv::Output output;
//     if (SpectralSubdiv::Refine(input, output)) {
//         // output.points and output.triangleIndices are ready
//     }
//
//   Thread safety: Refine() is stateless — safe to call concurrently
//   on different inputs.
// ---------------------------------------------------------------------------

#ifdef SPECTRAL_HAS_OSD

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralSubdiv
{
public:
    enum class Scheme {
        CatmullClark,
        Loop,
        Bilinear,
        None           // pass-through — no subdivision
    };

    struct Input {
        VtVec3fArray        points;              // coarse vertex positions
        VtIntArray          faceVertexCounts;     // per-face vertex count
        VtIntArray          faceVertexIndices;    // flat vertex indices
        Scheme              scheme = Scheme::CatmullClark;
        int                 level  = 2;           // refinement level (1–4)

        // Optional: coarse per-vertex normals (will be refined too)
        VtVec3fArray        normals;

        // Optional: coarse UVs (face-varying or vertex)
        VtVec2fArray        uvs;
        VtIntArray          uvIndices;    // face-varying UV indices (if empty, use faceVertexIndices)
        bool                uvIsFaceVarying = true;

        // Optional: crease edges (pairs of vertex indices + sharpness)
        VtIntArray          creaseIndices;      // pairs: [v0,v1, v2,v3, ...]
        VtFloatArray        creaseSharpnesses;  // one per crease edge

        // Optional: corner vertices (single vertex index + sharpness)
        VtIntArray          cornerIndices;
        VtFloatArray        cornerSharpnesses;
    };

    struct Output {
        VtVec3fArray        points;              // refined vertex positions
        VtVec3fArray        normals;             // refined per-vertex normals
        std::vector<int>    triangleIndices;     // flat: [v0,v1,v2, v0,v1,v2, ...]

        // Refined UVs — face-varying (one per face-vertex, same count as triangle indices)
        VtVec2fArray        uvs;
        std::vector<int>    uvIndices;           // UV index per triangle vertex
    };

    /// Refine a coarse mesh to the specified subdivision level.
    /// Returns true on success, false if the mesh can't be refined
    /// (e.g. degenerate topology, unsupported scheme).
    static bool Refine(const Input& input, Output& output);

    /// Convert a USD subdivision scheme token to our enum.
    /// "catmullClark" → CatmullClark, "loop" → Loop,
    /// "bilinear" → Bilinear, anything else → None.
    static Scheme SchemeFromToken(const std::string& token);
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_OSD
