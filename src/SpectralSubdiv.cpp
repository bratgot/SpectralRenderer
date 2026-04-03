#ifdef SPECTRAL_HAS_OSD

#include "SpectralSubdiv.h"

#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/far/primvarRefiner.h>

#include <cstdio>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

using namespace OpenSubdiv;

// ---------------------------------------------------------------------------
// Vertex adapter — OpenSubdiv needs Clear() and AddWithWeight()
// ---------------------------------------------------------------------------
struct OsdVertex3f {
    float x, y, z;

    OsdVertex3f() : x(0), y(0), z(0) {}
    OsdVertex3f(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    OsdVertex3f(const GfVec3f& v) : x(v[0]), y(v[1]), z(v[2]) {}

    void Clear(void* = nullptr) { x = y = z = 0.f; }

    void AddWithWeight(const OsdVertex3f& src, float weight) {
        x += src.x * weight;
        y += src.y * weight;
        z += src.z * weight;
    }

    GfVec3f ToVec3f() const { return GfVec3f(x, y, z); }
};

// UV adapter for face-varying interpolation
struct OsdVertex2f {
    float u, v;

    OsdVertex2f() : u(0), v(0) {}
    OsdVertex2f(float _u, float _v) : u(_u), v(_v) {}
    OsdVertex2f(const GfVec2f& uv) : u(uv[0]), v(uv[1]) {}

    void Clear(void* = nullptr) { u = v = 0.f; }

    void AddWithWeight(const OsdVertex2f& src, float weight) {
        u += src.u * weight;
        v += src.v * weight;
    }

    GfVec2f ToVec2f() const { return GfVec2f(u, v); }
};

// ---------------------------------------------------------------------------
// SchemeFromToken
// ---------------------------------------------------------------------------
SpectralSubdiv::Scheme
SpectralSubdiv::SchemeFromToken(const std::string& token)
{
    if (token == "catmullClark" || token == "catmark") return Scheme::CatmullClark;
    if (token == "loop")                               return Scheme::Loop;
    if (token == "bilinear")                           return Scheme::Bilinear;
    return Scheme::None;
}

// ---------------------------------------------------------------------------
// Refine
// ---------------------------------------------------------------------------
bool SpectralSubdiv::Refine(const Input& input, Output& output)
{
    if (input.points.empty() || input.faceVertexCounts.empty() ||
        input.faceVertexIndices.empty()) {
        return false;
    }

    if (input.scheme == Scheme::None || input.level <= 0) {
        return false;  // caller should skip subdivision
    }

    // Clamp level to a sane range
    const int level = std::max(1, std::min(input.level, 4));

    // Map our scheme enum to OpenSubdiv's
    Sdc::SchemeType sdcScheme;
    switch (input.scheme) {
        case Scheme::CatmullClark: sdcScheme = Sdc::SCHEME_CATMARK;  break;
        case Scheme::Loop:         sdcScheme = Sdc::SCHEME_LOOP;     break;
        case Scheme::Bilinear:     sdcScheme = Sdc::SCHEME_BILINEAR; break;
        default: return false;
    }

    // Loop scheme requires all-triangle input
    if (sdcScheme == Sdc::SCHEME_LOOP) {
        for (int i = 0; i < static_cast<int>(input.faceVertexCounts.size()); ++i) {
            if (input.faceVertexCounts[i] != 3) {
                fprintf(stderr, "SpectralSubdiv: Loop scheme requires triangles, "
                        "face %d has %d verts\n", i, input.faceVertexCounts[i]);
                return false;
            }
        }
    }

    // ------------------------------------------------------------------
    // Build topology descriptor
    // ------------------------------------------------------------------
    Far::TopologyDescriptor desc;
    desc.numVertices        = static_cast<int>(input.points.size());
    desc.numFaces           = static_cast<int>(input.faceVertexCounts.size());
    desc.numVertsPerFace    = input.faceVertexCounts.cdata();
    desc.vertIndicesPerFace = input.faceVertexIndices.cdata();

    // Face-varying UV channel
    Far::TopologyDescriptor::FVarChannel uvChannel;
    bool hasUVs = !input.uvs.empty();
    if (hasUVs) {
        uvChannel.numValues = static_cast<int>(input.uvs.size());
        // If separate UV indices provided, use them; otherwise use vertex indices
        if (!input.uvIndices.empty()) {
            uvChannel.valueIndices = input.uvIndices.cdata();
        } else {
            uvChannel.valueIndices = input.faceVertexIndices.cdata();
        }
        desc.numFVarChannels = 1;
        desc.fvarChannels    = &uvChannel;
    }

    // Crease edges
    if (!input.creaseIndices.empty() && !input.creaseSharpnesses.empty()) {
        desc.numCreases = static_cast<int>(input.creaseSharpnesses.size());
        desc.creaseVertexIndexPairs = input.creaseIndices.cdata();
        desc.creaseWeights = input.creaseSharpnesses.cdata();
    }

    // Corner vertices
    if (!input.cornerIndices.empty() && !input.cornerSharpnesses.empty()) {
        desc.numCorners = static_cast<int>(input.cornerIndices.size());
        desc.cornerVertexIndices = input.cornerIndices.cdata();
        desc.cornerWeights = input.cornerSharpnesses.cdata();
    }

    // ------------------------------------------------------------------
    // Create topology refiner
    // ------------------------------------------------------------------
    Sdc::Options sdcOptions;
    sdcOptions.SetVtxBoundaryInterpolation(
        Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER);

    Far::TopologyRefiner* refiner =
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc,
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options(
                sdcScheme, sdcOptions));

    if (!refiner) {
        fprintf(stderr, "SpectralSubdiv: failed to create topology refiner\n");
        return false;
    }

    // Uniform refinement
    Far::TopologyRefiner::UniformOptions refineOptions(level);
    refineOptions.fullTopologyInLastLevel = true;
    refiner->RefineUniform(refineOptions);

    // ------------------------------------------------------------------
    // Interpolate positions through refinement levels
    // ------------------------------------------------------------------
    const int coarseCount = static_cast<int>(input.points.size());
    int totalVerts = coarseCount;
    for (int l = 1; l <= level; ++l) {
        totalVerts += refiner->GetLevel(l).GetNumVertices();
    }

    std::vector<OsdVertex3f> vertBuffer(totalVerts);

    // Copy coarse positions
    for (int i = 0; i < coarseCount; ++i) {
        vertBuffer[i] = OsdVertex3f(input.points[i]);
    }

    // Refine level by level
    Far::PrimvarRefiner primvarRefiner(*refiner);
    OsdVertex3f* src = vertBuffer.data();
    for (int l = 1; l <= level; ++l) {
        OsdVertex3f* dst = src + refiner->GetLevel(l - 1).GetNumVertices();
        primvarRefiner.Interpolate(l, src, dst);
        src = dst;
    }

    // src now points to the start of the finest level's vertices
    const int fineVertCount = refiner->GetLevel(level).GetNumVertices();

    // ------------------------------------------------------------------
    // Optionally refine normals
    // ------------------------------------------------------------------
    std::vector<OsdVertex3f> normBuffer;
    bool hasNormals = !input.normals.empty() &&
                      static_cast<int>(input.normals.size()) == coarseCount;
    if (hasNormals) {
        normBuffer.resize(totalVerts);
        for (int i = 0; i < coarseCount; ++i) {
            normBuffer[i] = OsdVertex3f(input.normals[i]);
        }
        OsdVertex3f* nsrc = normBuffer.data();
        for (int l = 1; l <= level; ++l) {
            OsdVertex3f* ndst = nsrc + refiner->GetLevel(l - 1).GetNumVertices();
            primvarRefiner.Interpolate(l, nsrc, ndst);
            nsrc = ndst;
        }
    }

    // ------------------------------------------------------------------
    // Interpolate face-varying UVs
    // ------------------------------------------------------------------
    std::vector<OsdVertex2f> uvBuffer;
    int totalFVarValues = 0;
    if (hasUVs) {
        int coarseFVarCount = static_cast<int>(input.uvs.size());
        totalFVarValues = coarseFVarCount;
        for (int l = 1; l <= level; ++l) {
            totalFVarValues += refiner->GetLevel(l).GetNumFVarValues(0);
        }
        uvBuffer.resize(totalFVarValues);
        for (int i = 0; i < coarseFVarCount; ++i) {
            uvBuffer[i] = OsdVertex2f(input.uvs[i]);
        }
        OsdVertex2f* usrc = uvBuffer.data();
        for (int l = 1; l <= level; ++l) {
            OsdVertex2f* udst = usrc + refiner->GetLevel(l - 1).GetNumFVarValues(0);
            primvarRefiner.InterpolateFaceVarying(l, usrc, udst, 0);
            usrc = udst;
        }
    }

    // ------------------------------------------------------------------
    // Extract refined positions
    // ------------------------------------------------------------------
    output.points.resize(fineVertCount);
    for (int i = 0; i < fineVertCount; ++i) {
        output.points[i] = src[i].ToVec3f();
    }

    if (hasNormals) {
        OsdVertex3f* nsrc2 = normBuffer.data();
        for (int l = 0; l < level; ++l) {
            nsrc2 += refiner->GetLevel(l).GetNumVertices();
        }
        output.normals.resize(fineVertCount);
        for (int i = 0; i < fineVertCount; ++i) {
            GfVec3f n = nsrc2[i].ToVec3f();
            float len = n.GetLength();
            output.normals[i] = (len > 1e-6f) ? n / len : GfVec3f(0, 1, 0);
        }
    }

    // ------------------------------------------------------------------
    // Extract refined topology and triangulate
    //
    // At the finest level, Catmull-Clark produces all quads,
    // Loop produces all triangles, Bilinear produces all quads.
    // ------------------------------------------------------------------
    Far::TopologyLevel const& fineLevel = refiner->GetLevel(level);
    const int numFaces = fineLevel.GetNumFaces();

    output.triangleIndices.clear();
    output.triangleIndices.reserve(numFaces * 6);  // worst case: all quads

    // Get refined face-varying UVs
    OsdVertex2f* fineUVs = nullptr;
    if (hasUVs) {
        // Navigate to the finest level's UV data
        fineUVs = uvBuffer.data();
        for (int l = 0; l < level; ++l) {
            fineUVs += refiner->GetLevel(l).GetNumFVarValues(0);
        }
        int fineFVarCount = fineLevel.GetNumFVarValues(0);
        output.uvs.resize(fineFVarCount);
        for (int i = 0; i < fineFVarCount; ++i) {
            output.uvs[i] = fineUVs[i].ToVec2f();
        }
    }

    for (int fi = 0; fi < numFaces; ++fi) {
        Far::ConstIndexArray faceVerts = fineLevel.GetFaceVertices(fi);
        const int nv = faceVerts.size();

        if (nv < 3) continue;

        // Get face-varying UV indices for this face
        Far::ConstIndexArray faceUVs;
        if (hasUVs) {
            faceUVs = fineLevel.GetFaceFVarValues(fi, 0);
        }

        // Fan-triangulate: (0,1,2), (0,2,3), (0,3,4), ...
        for (int ti = 0; ti < nv - 2; ++ti) {
            output.triangleIndices.push_back(faceVerts[0]);
            output.triangleIndices.push_back(faceVerts[ti + 1]);
            output.triangleIndices.push_back(faceVerts[ti + 2]);

            if (hasUVs && faceUVs.size() > 0) {
                output.uvIndices.push_back(faceUVs[0]);
                output.uvIndices.push_back(faceUVs[ti + 1]);
                output.uvIndices.push_back(faceUVs[ti + 2]);
            }
        }
    }

    fprintf(stderr, "SpectralSubdiv: refined %d coarse verts → %d fine verts, "
            "%d triangles (level %d)\n",
            coarseCount, fineVertCount,
            static_cast<int>(output.triangleIndices.size() / 3), level);

    delete refiner;
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_OSD
