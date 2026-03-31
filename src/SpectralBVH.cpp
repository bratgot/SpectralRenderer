#ifdef SPECTRAL_HAS_EMBREE

#include "SpectralBVH.h"

#include <pxr/base/tf/diagnostic.h>

#include <cstring>
#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Embree error callback
// ---------------------------------------------------------------------------
static void _EmbreeErrorFunc(void* /*userPtr*/, RTCError code, const char* str)
{
    fprintf(stderr, "SpectralBVH: Embree error %d: %s\n", (int)code, str ? str : "(null)");
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
SpectralBVH::SpectralBVH() = default;

SpectralBVH::~SpectralBVH()
{
    Release();
}

SpectralBVH::SpectralBVH(SpectralBVH&& other) noexcept
    : _rtcDevice(other._rtcDevice)
    , _rtcScene(other._rtcScene)
    , _triangles(std::move(other._triangles))
{
    other._rtcDevice = nullptr;
    other._rtcScene  = nullptr;
}

SpectralBVH& SpectralBVH::operator=(SpectralBVH&& other) noexcept
{
    if (this != &other) {
        Release();
        _rtcDevice = other._rtcDevice;
        _rtcScene  = other._rtcScene;
        _triangles = std::move(other._triangles);
        other._rtcDevice = nullptr;
        other._rtcScene  = nullptr;
    }
    return *this;
}

void SpectralBVH::Release()
{
    if (_rtcScene) {
        rtcReleaseScene(_rtcScene);
        _rtcScene = nullptr;
    }
    if (_rtcDevice) {
        rtcReleaseDevice(_rtcDevice);
        _rtcDevice = nullptr;
    }
    _triangles.clear();
}

// ---------------------------------------------------------------------------
// Build — flatten all visible triangles into a single Embree geometry
// ---------------------------------------------------------------------------
void SpectralBVH::Build(const SpectralScene& scene)
{
    Release();

    // Collect triangle pointers (we need these for shading after hit)
    _triangles.clear();
    _triangles.reserve(scene.TotalTriangles());
    for (auto& kv : scene.GetMeshes()) {
        if (!kv.second.visible) continue;
        for (auto& tri : kv.second.triangles) {
            _triangles.push_back(&tri);
        }
    }

    if (_triangles.empty()) {
        fprintf(stderr, "SpectralBVH: no triangles to build\n");
        return;
    }

    // Create Embree device
    _rtcDevice = rtcNewDevice(nullptr);
    if (!_rtcDevice) {
        fprintf(stderr, "SpectralBVH: rtcNewDevice failed\n");
        return;
    }
    rtcSetDeviceErrorFunction(_rtcDevice, _EmbreeErrorFunc, nullptr);

    // Create scene
    _rtcScene = rtcNewScene(_rtcDevice);
    rtcSetSceneFlags(_rtcScene, RTC_SCENE_FLAG_COMPACT);
    rtcSetSceneBuildQuality(_rtcScene, RTC_BUILD_QUALITY_HIGH);

    // Create a single triangle mesh geometry
    RTCGeometry geom = rtcNewGeometry(_rtcDevice, RTC_GEOMETRY_TYPE_TRIANGLE);

    const size_t numTris = _triangles.size();

    // Vertex buffer: 3 vertices per triangle (non-indexed, fully expanded)
    // Embree needs float[3] per vertex, 16-byte stride is fine for float[3]
    float* verts = (float*)rtcSetNewGeometryBuffer(
        geom, RTC_BUFFER_TYPE_VERTEX, 0,
        RTC_FORMAT_FLOAT3, sizeof(float) * 3,
        numTris * 3);

    // Index buffer: trivial 0,1,2, 3,4,5, ...
    unsigned* indices = (unsigned*)rtcSetNewGeometryBuffer(
        geom, RTC_BUFFER_TYPE_INDEX, 0,
        RTC_FORMAT_UINT3, sizeof(unsigned) * 3,
        numTris);

    if (!verts || !indices) {
        fprintf(stderr, "SpectralBVH: failed to allocate Embree buffers\n");
        rtcReleaseGeometry(geom);
        Release();
        return;
    }

    // Fill buffers
    for (size_t i = 0; i < numTris; ++i) {
        const SpectralTriangle* tri = _triangles[i];

        float* v0 = verts + i * 9;      // 3 verts * 3 floats
        float* v1 = v0 + 3;
        float* v2 = v0 + 6;

        v0[0] = tri->v0[0]; v0[1] = tri->v0[1]; v0[2] = tri->v0[2];
        v1[0] = tri->v1[0]; v1[1] = tri->v1[1]; v1[2] = tri->v1[2];
        v2[0] = tri->v2[0]; v2[1] = tri->v2[1]; v2[2] = tri->v2[2];

        unsigned* idx = indices + i * 3;
        idx[0] = static_cast<unsigned>(i * 3);
        idx[1] = static_cast<unsigned>(i * 3 + 1);
        idx[2] = static_cast<unsigned>(i * 3 + 2);
    }

    rtcCommitGeometry(geom);
    rtcAttachGeometry(_rtcScene, geom);
    rtcReleaseGeometry(geom);  // scene holds a reference

    rtcCommitScene(_rtcScene);

    fprintf(stderr, "SpectralBVH: built BVH for %zu triangles\n", numTris);
}

// ---------------------------------------------------------------------------
// Intersect — single ray query, thread-safe
// ---------------------------------------------------------------------------
SpectralBVH::Hit SpectralBVH::Intersect(const GfRay& ray) const
{
    Hit result;
    if (!_rtcScene || _triangles.empty()) return result;

    const GfVec3d orig = ray.GetStartPoint();
    const GfVec3d dir  = ray.GetDirection();

    RTCRayHit rayhit;
    memset(&rayhit, 0, sizeof(rayhit));

    rayhit.ray.org_x = static_cast<float>(orig[0]);
    rayhit.ray.org_y = static_cast<float>(orig[1]);
    rayhit.ray.org_z = static_cast<float>(orig[2]);
    rayhit.ray.dir_x = static_cast<float>(dir[0]);
    rayhit.ray.dir_y = static_cast<float>(dir[1]);
    rayhit.ray.dir_z = static_cast<float>(dir[2]);
    rayhit.ray.tnear  = 1e-4f;
    rayhit.ray.tfar   = 1e30f;
    rayhit.ray.mask   = 0xFFFFFFFF;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

    rtcIntersect1(_rtcScene, &rayhit);

    if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
        unsigned primID = rayhit.hit.primID;
        if (primID < _triangles.size()) {
            result.t   = rayhit.ray.tfar;
            result.u   = rayhit.hit.u;
            result.v   = rayhit.hit.v;
            result.tri = _triangles[primID];
        }
    }

    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_EMBREE
