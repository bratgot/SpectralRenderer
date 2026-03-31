#include "SpectralRenderIop.h"

#include <DDImage/Scene.h>
#include <DDImage/GeometryList.h>
#include <DDImage/GeoInfo.h>
#include <DDImage/Primitive.h>
#include <DDImage/Point.h>
#include <DDImage/Matrix4.h>
#include <DDImage/Attribute.h>
#include <DDImage/Channel.h>
#include <DDImage/Black.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/path.h>

#include <cmath>
#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

// ---------------------------------------------------------------------------
// Class name constant
// ---------------------------------------------------------------------------
const char* const SpectralRenderIop::CLASS = "SpectralRender";

// ---------------------------------------------------------------------------
// Free constructor function — matches the pattern Nuke 17 expects.
// SimpleBlur example uses this exact pattern: a free function returning Op*.
// ---------------------------------------------------------------------------
static Op* SpectralRenderIopCreate(Node* node)
{
    return new SpectralRenderIop(node);
}

// ---------------------------------------------------------------------------
// Registration — Op::Description with 2-arg constructor (name, ctor).
//
// Key differences from the broken version:
//   1. Op::Description, NOT Iop::Description
//   2. Free function ctor, NOT static member function
//   3. Class at global scope, NOT inside namespace DD::Image
// ---------------------------------------------------------------------------
const Op::Description SpectralRenderIop::description(
    SpectralRenderIop::CLASS,
    SpectralRenderIopCreate
);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
SpectralRenderIop::SpectralRenderIop(Node* node)
    : Iop(node)
    , _scene(std::make_unique<pxr::SpectralScene>())
{
}

SpectralRenderIop::~SpectralRenderIop() = default;

const char* SpectralRenderIop::node_help() const
{
    return
        "SpectralRender — physically-based spectral path tracer.\n\n"
        "Input 0 (Scene): any 3D GeoOp.\n"
        "Input 1 (Camera): optional CameraOp. Defaults to 50mm perspective.\n\n"
        "Outputs RGBA float32 image data into the Nuke graph.\n"
        "Connect downstream to Grade, Write, or any other Iop.";
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
const char* SpectralRenderIop::input_label(int idx, char*) const
{
    if (idx == 0) return "Scene";
    if (idx == 1) return "Camera";
    return "";
}

bool SpectralRenderIop::test_input(int idx, Op* op) const
{
    if (idx == 0) return dynamic_cast<GeoOp*>(op) != nullptr;
    if (idx == 1) return op == nullptr || dynamic_cast<CameraOp*>(op) != nullptr;
    return false;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------
void SpectralRenderIop::knobs(Knob_Callback f)
{
    Format_knob(f, &_outputFormat, "format", "format");
    Divider(f, "Spectral render settings");
    Int_knob(f, &_samples,    "samples",    "samples per pixel"); SetRange(f, 1, 256);
    Int_knob(f, &_maxBounces, "max_bounces","max bounces");        SetRange(f, 1, 16);
    Int_knob(f, &_tileSize,   "tile_size",  "tile size");          SetRange(f, 16, 256);
}

int SpectralRenderIop::knob_changed(Knob* k)
{
    _frameReady.store(false);
    return Iop::knob_changed(k);
}

// ---------------------------------------------------------------------------
// _validate
// ---------------------------------------------------------------------------
void SpectralRenderIop::_validate(bool forReal)
{
    if (forReal) {
        _SyncScene();
        _frameReady.store(false);
    }

    const Format* fmtPtr = _outputFormat.format();
    if (!fmtPtr) {
        // No format selected yet — fall back to root format
        fmtPtr = &input_format();
    }
    const Format& fmt = *fmtPtr;
    info_.format(fmt);
    info_.set(fmt);
    info_.channels(Mask_RGBA);

    Iop::_validate(forReal);
}

void SpectralRenderIop::_request(int, int, int, int, ChannelMask, int) {}

// ---------------------------------------------------------------------------
// engine
// ---------------------------------------------------------------------------
void SpectralRenderIop::engine(
    int y, int x, int r, ChannelMask channels, Row& row)
{
    _EnsureFrameRendered();

    const int W = static_cast<int>(_fbWidth);
    const int H = static_cast<int>(_fbHeight);
    int bufY = H - 1 - y;

    if (bufY < 0 || bufY >= H || _frameBuffer.empty()) {
        row.erase(channels);
        return;
    }

    const float* srcRow = _frameBuffer.data() + bufY * W * 4;

    auto copyChannel = [&](Channel ch, int comp) {
        if (!(channels & ch)) return;
        float* out = row.writable(ch);
        for (int px = x; px < r; ++px)
            out[px] = (px >= 0 && px < W) ? srcRow[px * 4 + comp] : 0.f;
    };

    copyChannel(Chan_Red,   0);
    copyChannel(Chan_Green, 1);
    copyChannel(Chan_Blue,  2);
    copyChannel(Chan_Alpha, 3);
}

// ---------------------------------------------------------------------------
// _SyncScene
// ---------------------------------------------------------------------------
void SpectralRenderIop::_SyncScene()
{
    _scene = std::make_unique<pxr::SpectralScene>();

    GeoOp* geoIn = dynamic_cast<GeoOp*>(input(0));
    if (!geoIn) return;

    geoIn->validate(true);

    // Build the 3-D scene from the GeoOp.
    // build_scene populates the Scene's object list.
    Scene scene3d;
    geoIn->build_scene(scene3d);

    GeometryList* geoListPtr = scene3d.object_list();
    if (!geoListPtr) return;                       // <-- was crashing here
    GeometryList& geoList = *geoListPtr;
    if (geoList.size() == 0) return;

    for (unsigned int objIdx = 0; objIdx < geoList.size(); ++objIdx) {
        GeoInfo& geo = geoList[objIdx];

        const DD::Image::Matrix4& m4 = geo.matrix;
        GfMatrix4d L2W(
            m4[0][0], m4[0][1], m4[0][2], m4[0][3],
            m4[1][0], m4[1][1], m4[1][2], m4[1][3],
            m4[2][0], m4[2][1], m4[2][2], m4[2][3],
            m4[3][0], m4[3][1], m4[3][2], m4[3][3]
        );
        GfMatrix4d normalMat = L2W.GetInverse().GetTranspose();

        const PointList* pts = geo.point_list();
        if (!pts || pts->size() == 0) continue;
        const unsigned int numPoints = static_cast<unsigned int>(pts->size());

        // Normal attribute — check interpolation group so we index safely.
        // Group_Points  → index by point index  (same as vertex index here)
        // Group_Vertices → index by absolute vertex offset within the prim
        // Anything else → skip authored normals, use geometric fallback
        const AttribContext* normCtx = geo.get_attribcontext("N");
        const Attribute* normAttrib = nullptr;
        bool normalsPerVertex = false;   // true = index by vertex offset
        if (normCtx && normCtx->attribute) {
            normAttrib = &(*normCtx->attribute);
            normalsPerVertex = (normCtx->group == Group_Vertices);
            // Group_Points or Group_Object are indexed by point id
        }

        pxr::SpectralMeshData data;
        data.id      = SdfPath("/geo_" + std::to_string(objIdx));
        data.visible = true;

        auto xfPt = [&](const Vector3& p) -> GfVec3f {
            Vector4 w = m4 * Vector4(p.x, p.y, p.z, 1.f);
            return GfVec3f(w.x, w.y, w.z);
        };
        auto xfNorm = [&](const GfVec3f& n) -> GfVec3f {
            GfVec3f xn(normalMat.TransformDir(GfVec3d(n)));
            float len = xn.GetLength();
            return (len > 1e-6f) ? xn / len : GfVec3f(0.f, 1.f, 0.f);
        };

        for (unsigned int primIdx = 0; primIdx < geo.primitives(); ++primIdx) {
            const Primitive* prim = geo.primitive(primIdx);
            if (!prim) continue;

            const unsigned int numFaces = prim->faces();
            if (numFaces == 0) {
                // Some primitives (e.g. Particles) report 0 faces but
                // still have vertices.  Skip them for now.
                continue;
            }

            // Track cumulative vertex offset across faces.
            // prim->vertex(vertexOffset + k) gives the point index of
            // the k-th vertex of the current face.
            unsigned int vertexOffset = 0;

            for (unsigned int fi = 0; fi < numFaces; ++fi) {
                const unsigned int nv = prim->face_vertices(fi);
                if (nv < 3) {
                    vertexOffset += nv;
                    continue;
                }

                // Fan-triangulate: (0, 1, 2), (0, 2, 3), (0, 3, 4) ...
                for (unsigned int ti = 0; ti < nv - 2; ++ti) {
                    // Absolute vertex indices within the primitive
                    const unsigned int vi0 = vertexOffset + 0;
                    const unsigned int vi1 = vertexOffset + ti + 1;
                    const unsigned int vi2 = vertexOffset + ti + 2;

                    // Safety: make sure vertex() won't go out of range
                    if (vi2 >= prim->vertices()) break;

                    // Point indices into the PointList
                    const unsigned int pi0 = prim->vertex(vi0);
                    const unsigned int pi1 = prim->vertex(vi1);
                    const unsigned int pi2 = prim->vertex(vi2);
                    if (pi0 >= numPoints || pi1 >= numPoints || pi2 >= numPoints)
                        continue;

                    pxr::SpectralTriangle tri;
                    tri.v0 = xfPt((*pts)[pi0]);
                    tri.v1 = xfPt((*pts)[pi1]);
                    tri.v2 = xfPt((*pts)[pi2]);

                    // Geometric face normal (always available)
                    GfVec3f e0 = tri.v1 - tri.v0;
                    GfVec3f e1 = tri.v2 - tri.v0;
                    GfVec3f fn = GfCross(e0, e1);
                    float   fl = fn.GetLength();
                    tri.faceNormal = (fl > 1e-8f) ? fn / fl : GfVec3f(0.f, 1.f, 0.f);

                    // Authored normals — use correct index depending on group
                    bool gotNormals = false;
                    if (normAttrib) {
                        auto safeNormal = [&](unsigned int idx) -> GfVec3f {
                            if (idx >= normAttrib->size()) return tri.faceNormal;
                            Vector3 n = normAttrib->vector3(idx);
                            return xfNorm(GfVec3f(n.x, n.y, n.z));
                        };

                        if (normalsPerVertex) {
                            // Group_Vertices: index by absolute vertex offset
                            tri.n0 = safeNormal(vi0);
                            tri.n1 = safeNormal(vi1);
                            tri.n2 = safeNormal(vi2);
                            gotNormals = true;
                        } else {
                            // Group_Points: index by point index
                            tri.n0 = safeNormal(pi0);
                            tri.n1 = safeNormal(pi1);
                            tri.n2 = safeNormal(pi2);
                            gotNormals = true;
                        }
                    }

                    if (!gotNormals) {
                        tri.n0 = tri.n1 = tri.n2 = tri.faceNormal;
                    }

                    data.triangles.push_back(tri);
                }

                vertexOffset += nv;
            }
        }
        if (!data.triangles.empty())
            _scene->SetMeshData(data.id, std::move(data));
    }
}

// ---------------------------------------------------------------------------
// _BuildCamera
// ---------------------------------------------------------------------------
SpectralCamera SpectralRenderIop::_BuildCamera() const
{
    SpectralCamera cam;
    const Format* fmtPtr = _outputFormat.format();
    if (fmtPtr) {
        cam.imageWidth  = static_cast<unsigned int>(fmtPtr->width());
        cam.imageHeight = static_cast<unsigned int>(fmtPtr->height());
    }
    // Ensure we never get zero dimensions (division by zero in projection)
    if (cam.imageWidth  == 0) cam.imageWidth  = 1920;
    if (cam.imageHeight == 0) cam.imageHeight = 1080;

    CameraOp* camIn = dynamic_cast<CameraOp*>(input(1));
    if (camIn) {
        camIn->validate(true);
        const DD::Image::Matrix4& cw = camIn->matrix();
        cam.viewToWorld = GfMatrix4d(
            cw[0][0], cw[0][1], cw[0][2], cw[0][3],
            cw[1][0], cw[1][1], cw[1][2], cw[1][3],
            cw[2][0], cw[2][1], cw[2][2], cw[2][3],
            cw[3][0], cw[3][1], cw[3][2], cw[3][3]
        );
        const DD::Image::Matrix4& pr = camIn->projection();
        cam.projInverse = GfMatrix4d(
            pr[0][0], pr[0][1], pr[0][2], pr[0][3],
            pr[1][0], pr[1][1], pr[1][2], pr[1][3],
            pr[2][0], pr[2][1], pr[2][2], pr[2][3],
            pr[3][0], pr[3][1], pr[3][2], pr[3][3]
        ).GetInverse();
    } else {
        cam.viewToWorld = GfMatrix4d(1.0);
        const double fov    = 50.0 * M_PI / 180.0;
        const double aspect = double(cam.imageWidth) / double(cam.imageHeight);
        const double near_  = 0.1, far_ = 10000.0;
        const double f      = 1.0 / std::tan(fov * 0.5);
        GfMatrix4d proj(0.0);
        proj[0][0] = f / aspect;
        proj[1][1] = f;
        proj[2][2] = (far_ + near_) / (near_ - far_);
        proj[2][3] = -1.0;
        proj[3][2] = (2.0 * far_ * near_) / (near_ - far_);
        cam.projInverse = proj.GetInverse();
    }
    return cam;
}

// ---------------------------------------------------------------------------
// _EnsureFrameRendered
// ---------------------------------------------------------------------------
void SpectralRenderIop::_EnsureFrameRendered()
{
    if (_frameReady.load()) return;
    std::lock_guard<std::mutex> lock(_renderMutex);
    if (_frameReady.load()) return;

    const Format* fmtPtr = _outputFormat.format();
    if (!fmtPtr) { _frameReady.store(true); return; }
    const unsigned int W = static_cast<unsigned int>(fmtPtr->width());
    const unsigned int H = static_cast<unsigned int>(fmtPtr->height());
    if (W == 0 || H == 0) { _frameReady.store(true); return; }

    _fbWidth  = W;
    _fbHeight = H;
    _frameBuffer.assign(size_t(W) * H * 4, 0.f);

    SpectralCamera cam = _BuildCamera();
    cam.imageWidth  = W;
    cam.imageHeight = H;

    SpectralIntegrator::RenderFrame(*_scene, cam, _frameBuffer.data());
    _frameReady.store(true);
}
