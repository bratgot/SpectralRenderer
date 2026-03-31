#include "SpectralRenderIop.h"

// PXR — USD stage traversal
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/diagnostic.h>

#include <DDImage/Channel.h>

#include <cmath>
#include <cstdio>
#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

// ---------------------------------------------------------------------------
const char* const SpectralRenderIop::CLASS = "SpectralRender";

static Op* SpectralRenderIopCreate(Node* node)
{
    return new SpectralRenderIop(node);
}

const Op::Description SpectralRenderIop::description(
    SpectralRenderIop::CLASS,
    SpectralRenderIopCreate
);

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
        "Point the 'USD file' knob at a .usd/.usda/.usdc file.\n"
        "Optionally specify a camera prim path, or leave blank\n"
        "to auto-find the first camera (default 50mm if none).\n\n"
        "For interactive viewport, use the Hydra delegate instead\n"
        "(\"Spectral (CPU)\" in the renderer dropdown).";
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------
void SpectralRenderIop::knobs(Knob_Callback f)
{
    File_knob(f, &_usdFilePath, "usd_file", "USD file");
    Tooltip(f, "Path to a .usd, .usda, or .usdc file.");

    String_knob(f, &_cameraPath, "camera_path", "camera prim");
    Tooltip(f, "USD prim path e.g. /World/Camera. Leave blank to auto-find.");

    Int_knob(f, &_frame, "frame", "frame");

    Format_knob(f, &_outputFormat, "format", "format");

    Divider(f, "Render settings");
    Int_knob(f, &_samples,    "samples",     "samples per pixel"); SetRange(f, 1, 256);
    Int_knob(f, &_maxBounces, "max_bounces", "max bounces");       SetRange(f, 1, 16);
    Int_knob(f, &_tileSize,   "tile_size",   "tile size");         SetRange(f, 16, 256);
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
    const Format* fmtPtr = _outputFormat.format();
    if (!fmtPtr) {
        info_.channels(Mask_RGBA);
        return;
    }

    info_.format(*fmtPtr);
    info_.full_size_format(*fmtPtr);
    info_.set(*fmtPtr);
    info_.channels(Mask_RGBA);
    info_.black_outside(true);

    if (forReal) {
        _LoadStage();
        _frameReady.store(false);
    }
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
// _LoadStage — single stage open, loads meshes AND camera
// ---------------------------------------------------------------------------
void SpectralRenderIop::_LoadStage()
{
    _scene = std::make_unique<pxr::SpectralScene>();

    // Reset camera to default
    _camera = SpectralCamera();

    if (!_usdFilePath || _usdFilePath[0] == '\0') {
        fprintf(stderr, "SpectralRender: no USD file set\n");
        return;
    }

    fprintf(stderr, "SpectralRender: opening %s\n", _usdFilePath);

    // ------------------------------------------------------------------
    // Open stage
    // ------------------------------------------------------------------
    UsdStageRefPtr stage;
    try {
        stage = UsdStage::Open(std::string(_usdFilePath));
    } catch (const std::exception& e) {
        fprintf(stderr, "SpectralRender: exception opening stage: %s\n", e.what());
        return;
    } catch (...) {
        fprintf(stderr, "SpectralRender: unknown exception opening stage\n");
        return;
    }
    if (!stage) {
        fprintf(stderr, "SpectralRender: UsdStage::Open returned null for %s\n", _usdFilePath);
        return;
    }

    fprintf(stderr, "SpectralRender: stage opened OK\n");

    const UsdTimeCode timeCode(_frame);
    UsdGeomXformCache xfCache(timeCode);

    // ------------------------------------------------------------------
    // Traverse stage — collect meshes and find camera
    // ------------------------------------------------------------------
    int meshCount = 0;
    int totalTris = 0;
    UsdPrim cameraPrim;

    // If user specified a camera path, try that first
    if (_cameraPath && _cameraPath[0] != '\0') {
        UsdPrim p = stage->GetPrimAtPath(SdfPath(std::string(_cameraPath)));
        if (p.IsValid() && p.IsA<UsdGeomCamera>()) {
            cameraPrim = p;
            fprintf(stderr, "SpectralRender: using specified camera: %s\n", _cameraPath);
        } else {
            fprintf(stderr, "SpectralRender: camera not found at %s\n", _cameraPath);
        }
    }

    for (const UsdPrim& prim : stage->Traverse()) {
        // ----------------------------------------------------------
        // Auto-find first camera if not specified
        // ----------------------------------------------------------
        if (!cameraPrim.IsValid() && prim.IsA<UsdGeomCamera>()) {
            cameraPrim = prim;
            fprintf(stderr, "SpectralRender: auto-found camera: %s\n",
                    prim.GetPath().GetText());
        }

        // ----------------------------------------------------------
        // Process meshes
        // ----------------------------------------------------------
        if (!prim.IsA<UsdGeomMesh>()) continue;

        UsdGeomMesh mesh(prim);
        if (!mesh) continue;

        // Points
        VtVec3fArray points;
        mesh.GetPointsAttr().Get(&points, timeCode);
        if (points.empty()) {
            fprintf(stderr, "SpectralRender: mesh %s has no points, skipping\n",
                    prim.GetPath().GetText());
            continue;
        }

        // Topology
        VtIntArray faceVertexCounts;
        VtIntArray faceVertexIndices;
        mesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts, timeCode);
        mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, timeCode);
        if (faceVertexCounts.empty() || faceVertexIndices.empty()) {
            fprintf(stderr, "SpectralRender: mesh %s has no topology, skipping\n",
                    prim.GetPath().GetText());
            continue;
        }

        // Normals (optional)
        VtVec3fArray normals;
        mesh.GetNormalsAttr().Get(&normals, timeCode);
        TfToken normalsInterp = mesh.GetNormalsInterpolation();

        // World transform
        GfMatrix4d worldXf = xfCache.GetLocalToWorldTransform(prim);
        GfMatrix4d normalXf = worldXf.GetInverse().GetTranspose();

        const int numPoints = static_cast<int>(points.size());

        fprintf(stderr, "SpectralRender: mesh %s — %d points, %d faces, %d normals (%s)\n",
                prim.GetPath().GetText(),
                numPoints,
                static_cast<int>(faceVertexCounts.size()),
                static_cast<int>(normals.size()),
                normalsInterp.GetText());

        // Helpers
        auto xfPoint = [&](const GfVec3f& p) -> GfVec3f {
            return GfVec3f(worldXf.Transform(GfVec3d(p)));
        };
        auto xfNormal = [&](const GfVec3f& n) -> GfVec3f {
            GfVec3f xn = GfVec3f(normalXf.TransformDir(GfVec3d(n)));
            float len = xn.GetLength();
            return (len > 1e-6f) ? xn / len : GfVec3f(0.f, 1.f, 0.f);
        };

        pxr::SpectralMeshData data;
        data.id      = prim.GetPath();
        data.visible = true;

        // Fan-triangulate each face
        int vertexOffset = 0;
        for (int fi = 0; fi < static_cast<int>(faceVertexCounts.size()); ++fi) {
            const int nv = faceVertexCounts[fi];
            if (nv < 3) {
                vertexOffset += nv;
                continue;
            }

            for (int ti = 0; ti < nv - 2; ++ti) {
                const int idx0 = vertexOffset;
                const int idx1 = vertexOffset + ti + 1;
                const int idx2 = vertexOffset + ti + 2;

                if (idx2 >= static_cast<int>(faceVertexIndices.size())) break;

                const int pi0 = faceVertexIndices[idx0];
                const int pi1 = faceVertexIndices[idx1];
                const int pi2 = faceVertexIndices[idx2];

                if (pi0 < 0 || pi0 >= numPoints ||
                    pi1 < 0 || pi1 >= numPoints ||
                    pi2 < 0 || pi2 >= numPoints) continue;

                pxr::SpectralTriangle tri;
                tri.v0 = xfPoint(points[pi0]);
                tri.v1 = xfPoint(points[pi1]);
                tri.v2 = xfPoint(points[pi2]);

                // Face normal
                GfVec3f e0 = tri.v1 - tri.v0;
                GfVec3f e1 = tri.v2 - tri.v0;
                GfVec3f fn = GfCross(e0, e1);
                float fl = fn.GetLength();
                tri.faceNormal = (fl > 1e-8f) ? fn / fl : GfVec3f(0.f, 1.f, 0.f);

                // Authored normals
                bool gotNormals = false;
                if (!normals.empty()) {
                    if (normalsInterp == UsdGeomTokens->faceVarying) {
                        if (idx2 < static_cast<int>(normals.size())) {
                            tri.n0 = xfNormal(normals[idx0]);
                            tri.n1 = xfNormal(normals[idx1]);
                            tri.n2 = xfNormal(normals[idx2]);
                            gotNormals = true;
                        }
                    } else if (normalsInterp == UsdGeomTokens->vertex) {
                        if (pi0 < static_cast<int>(normals.size()) &&
                            pi1 < static_cast<int>(normals.size()) &&
                            pi2 < static_cast<int>(normals.size())) {
                            tri.n0 = xfNormal(normals[pi0]);
                            tri.n1 = xfNormal(normals[pi1]);
                            tri.n2 = xfNormal(normals[pi2]);
                            gotNormals = true;
                        }
                    } else if (normalsInterp == UsdGeomTokens->uniform) {
                        if (fi < static_cast<int>(normals.size())) {
                            GfVec3f fn2 = xfNormal(normals[fi]);
                            tri.n0 = tri.n1 = tri.n2 = fn2;
                            gotNormals = true;
                        }
                    }
                }

                if (!gotNormals) {
                    tri.n0 = tri.n1 = tri.n2 = tri.faceNormal;
                }

                data.triangles.push_back(tri);
                totalTris++;
            }

            vertexOffset += nv;
        }

        if (!data.triangles.empty()) {
            _scene->SetMeshData(data.id, std::move(data));
            meshCount++;
        }
    }

    fprintf(stderr, "SpectralRender: loaded %d meshes, %d triangles total\n",
            meshCount, totalTris);

    // ------------------------------------------------------------------
    // Build camera from the stage (or default)
    // ------------------------------------------------------------------
    const Format* fmtPtr = _outputFormat.format();
    unsigned int W = 1920, H = 1080;
    if (fmtPtr) {
        W = static_cast<unsigned int>(fmtPtr->width());
        H = static_cast<unsigned int>(fmtPtr->height());
    }
    if (W == 0) W = 1920;
    if (H == 0) H = 1080;

    _camera.imageWidth  = W;
    _camera.imageHeight = H;
    const double aspect = double(W) / double(H);

    bool foundCamera = false;
    if (cameraPrim.IsValid()) {
        try {
            UsdGeomCamera geomCam(cameraPrim);
            GfCamera gfCam = geomCam.GetCamera(timeCode);

            // Camera-to-world from xform cache
            GfMatrix4d camToWorld = xfCache.GetLocalToWorldTransform(cameraPrim);
            _camera.viewToWorld = camToWorld;

            // Projection from GfCamera frustum
            GfFrustum frustum = gfCam.GetFrustum();
            GfMatrix4d projMatrix = frustum.ComputeProjectionMatrix();
            _camera.projInverse = projMatrix.GetInverse();

            foundCamera = true;

            // Debug: print camera transform
            GfVec3d camPos = camToWorld.ExtractTranslation();
            fprintf(stderr, "SpectralRender: camera at (%.2f, %.2f, %.2f)\n",
                    camPos[0], camPos[1], camPos[2]);

            fprintf(stderr, "SpectralRender: projection matrix:\n");
            for (int row = 0; row < 4; row++) {
                fprintf(stderr, "  [%.4f, %.4f, %.4f, %.4f]\n",
                        projMatrix[row][0], projMatrix[row][1],
                        projMatrix[row][2], projMatrix[row][3]);
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "SpectralRender: camera exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "SpectralRender: camera unknown exception\n");
        }
    }

    if (!foundCamera) {
        fprintf(stderr, "SpectralRender: using default 50mm camera at origin\n");
        _camera.viewToWorld = GfMatrix4d(1.0);
        const double fov   = 50.0 * M_PI / 180.0;
        const double near_ = 0.1, far_ = 10000.0;
        const double f     = 1.0 / std::tan(fov * 0.5);
        GfMatrix4d proj(0.0);
        proj[0][0] = f / aspect;
        proj[1][1] = f;
        proj[2][2] = (far_ + near_) / (near_ - far_);
        proj[2][3] = -1.0;
        proj[3][2] = (2.0 * far_ * near_) / (near_ - far_);
        _camera.projInverse = proj.GetInverse();
    }

    // Debug: print first triangle if any
    if (totalTris > 0) {
        for (auto& kv : _scene->GetMeshes()) {
            if (kv.second.triangles.empty()) continue;
            auto& t = kv.second.triangles[0];
            fprintf(stderr, "SpectralRender: first triangle:\n");
            fprintf(stderr, "  v0=(%.3f, %.3f, %.3f)\n", t.v0[0], t.v0[1], t.v0[2]);
            fprintf(stderr, "  v1=(%.3f, %.3f, %.3f)\n", t.v1[0], t.v1[1], t.v1[2]);
            fprintf(stderr, "  v2=(%.3f, %.3f, %.3f)\n", t.v2[0], t.v2[1], t.v2[2]);
            break;
        }
    }
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

    // Use the camera built during _LoadStage — no second stage open
    SpectralCamera cam = _camera;
    cam.imageWidth  = W;
    cam.imageHeight = H;

    fprintf(stderr, "SpectralRender: rendering %dx%d, %zu triangles in scene\n",
            W, H, _scene->TotalTriangles());

    SpectralIntegrator::RenderFrame(*_scene, cam, _frameBuffer.data());
    _frameReady.store(true);

    fprintf(stderr, "SpectralRender: render complete\n");
}
