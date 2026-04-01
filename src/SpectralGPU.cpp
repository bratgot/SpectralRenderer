#ifdef SPECTRAL_HAS_OPTIX

#include "SpectralGPU.h"
#include "SpectralIntegrator.h"  // full SpectralCamera definition

#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include <cstdio>
#include <cstring>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
#define CUDA_CHECK(call)                                                     \
    do {                                                                     \
        cudaError_t err = call;                                              \
        if (err != cudaSuccess) {                                            \
            fprintf(stderr, "SpectralGPU: CUDA error %s at %s:%d\n",        \
                    cudaGetErrorString(err), __FILE__, __LINE__);            \
            return false;                                                    \
        }                                                                    \
    } while (0)

#define OPTIX_CHECK(call)                                                    \
    do {                                                                     \
        OptixResult res = call;                                              \
        if (res != OPTIX_SUCCESS) {                                          \
            fprintf(stderr, "SpectralGPU: OptiX error %d at %s:%d\n",       \
                    (int)res, __FILE__, __LINE__);                           \
            return false;                                                    \
        }                                                                    \
    } while (0)

static void _OptixLogCallback(unsigned int level, const char* tag, const char* msg, void*)
{
    if (level < 4)  // skip verbose
        fprintf(stderr, "SpectralGPU [OptiX %s]: %s\n", tag, msg);
}

// SBT record template
template<typename T>
struct SbtRecord {
    __align__(OPTIX_SBT_RECORD_ALIGNMENT)
    char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    T data;
};

using RayGenSbtRecord   = SbtRecord<spectral_gpu::RayGenData>;
using MissSbtRecord     = SbtRecord<spectral_gpu::MissData>;
using HitGroupSbtRecord = SbtRecord<spectral_gpu::HitGroupData>;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
SpectralGPU::SpectralGPU() = default;

SpectralGPU::~SpectralGPU()
{
    Cleanup();
}

void SpectralGPU::Cleanup()
{
    _FreeBuffers();
    _FreeAccel();

    if (_sbtRaygenRecord)   { cudaFree(reinterpret_cast<void*>(_sbtRaygenRecord));   _sbtRaygenRecord = 0; }
    if (_sbtMissRecord)     { cudaFree(reinterpret_cast<void*>(_sbtMissRecord));     _sbtMissRecord = 0; }
    if (_sbtHitgroupRecord) { cudaFree(reinterpret_cast<void*>(_sbtHitgroupRecord)); _sbtHitgroupRecord = 0; }

    if (_hitgroupPG) { optixProgramGroupDestroy(_hitgroupPG); _hitgroupPG = nullptr; }
    if (_missPG)     { optixProgramGroupDestroy(_missPG);     _missPG = nullptr; }
    if (_raygenPG)   { optixProgramGroupDestroy(_raygenPG);   _raygenPG = nullptr; }
    if (_pipeline)   { optixPipelineDestroy(_pipeline);       _pipeline = nullptr; }
    if (_module)     { optixModuleDestroy(_module);           _module = nullptr; }
    if (_optixContext) { optixDeviceContextDestroy(_optixContext); _optixContext = nullptr; }

    memset(&_sbt, 0, sizeof(_sbt));
}

void SpectralGPU::_FreeAccel()
{
    if (_gasBuffer) { cudaFree(reinterpret_cast<void*>(_gasBuffer)); _gasBuffer = 0; }
    _gasHandle = 0;
}

void SpectralGPU::_FreeBuffers()
{
    if (_d_framebuffer) { cudaFree(reinterpret_cast<void*>(_d_framebuffer)); _d_framebuffer = 0; }
    if (_d_depthbuffer) { cudaFree(reinterpret_cast<void*>(_d_depthbuffer)); _d_depthbuffer = 0; }
    if (_d_normals)     { cudaFree(reinterpret_cast<void*>(_d_normals));     _d_normals = 0; }
    if (_d_params)      { cudaFree(reinterpret_cast<void*>(_d_params));      _d_params = 0; }
    _allocW = _allocH = 0;
    _triCount = 0;
}

// ---------------------------------------------------------------------------
// Initialize — create OptiX context, compile module, build pipeline
// ---------------------------------------------------------------------------
bool SpectralGPU::Initialize(const std::string& ptxSource)
{
    Cleanup();

    // CUDA init
    CUDA_CHECK(cudaFree(nullptr));  // force CUDA context init
    CUcontext cuCtx = nullptr;
    cuCtxGetCurrent(&cuCtx);
    if (!cuCtx) {
        fprintf(stderr, "SpectralGPU: no CUDA context\n");
        return false;
    }

    // OptiX init
    OPTIX_CHECK(optixInit());

    OptixDeviceContextOptions ctxOptions = {};
    ctxOptions.logCallbackFunction = _OptixLogCallback;
    ctxOptions.logCallbackLevel    = 3;  // warnings + errors
    OPTIX_CHECK(optixDeviceContextCreate(cuCtx, &ctxOptions, &_optixContext));

    fprintf(stderr, "SpectralGPU: OptiX context created\n");

    // Module
    OptixModuleCompileOptions moduleOptions = {};
    moduleOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    moduleOptions.optLevel         = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    moduleOptions.debugLevel       = OPTIX_COMPILE_DEBUG_LEVEL_NONE;

    OptixPipelineCompileOptions pipelineOptions = {};
    pipelineOptions.usesMotionBlur        = false;
    pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pipelineOptions.numPayloadValues      = 5;
    pipelineOptions.numAttributeValues    = 2;  // barycentrics
    pipelineOptions.pipelineLaunchParamsVariableName = "params";

    char log[2048];
    size_t logSize = sizeof(log);

    OPTIX_CHECK(optixModuleCreate(
        _optixContext, &moduleOptions, &pipelineOptions,
        ptxSource.c_str(), ptxSource.size(),
        log, &logSize, &_module));

    if (logSize > 1)
        fprintf(stderr, "SpectralGPU: module log: %s\n", log);

    fprintf(stderr, "SpectralGPU: module compiled\n");

    // Program groups
    OptixProgramGroupOptions pgOptions = {};

    // Raygen
    OptixProgramGroupDesc raygenDesc = {};
    raygenDesc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    raygenDesc.raygen.module = _module;
    raygenDesc.raygen.entryFunctionName = "__raygen__spectral";
    logSize = sizeof(log);
    OPTIX_CHECK(optixProgramGroupCreate(
        _optixContext, &raygenDesc, 1, &pgOptions, log, &logSize, &_raygenPG));

    // Miss
    OptixProgramGroupDesc missDesc = {};
    missDesc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    missDesc.miss.module = _module;
    missDesc.miss.entryFunctionName = "__miss__spectral";
    logSize = sizeof(log);
    OPTIX_CHECK(optixProgramGroupCreate(
        _optixContext, &missDesc, 1, &pgOptions, log, &logSize, &_missPG));

    // Hit group
    OptixProgramGroupDesc hitgroupDesc = {};
    hitgroupDesc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    hitgroupDesc.hitgroup.moduleCH = _module;
    hitgroupDesc.hitgroup.entryFunctionNameCH = "__closesthit__spectral";
    logSize = sizeof(log);
    OPTIX_CHECK(optixProgramGroupCreate(
        _optixContext, &hitgroupDesc, 1, &pgOptions, log, &logSize, &_hitgroupPG));

    fprintf(stderr, "SpectralGPU: program groups created\n");

    // Pipeline
    OptixProgramGroup programGroups[] = { _raygenPG, _missPG, _hitgroupPG };

    OptixPipelineLinkOptions linkOptions = {};
    linkOptions.maxTraceDepth = 1;

    logSize = sizeof(log);
    OPTIX_CHECK(optixPipelineCreate(
        _optixContext, &pipelineOptions, &linkOptions,
        programGroups, 3,
        log, &logSize, &_pipeline));

    // Stack sizes
    OptixStackSizes stackSizes = {};
    for (auto pg : programGroups) {
        OptixStackSizes ss;
        OPTIX_CHECK(optixProgramGroupGetStackSize(pg, &ss, _pipeline));
        stackSizes.cssRG = std::max(stackSizes.cssRG, ss.cssRG);
        stackSizes.cssMS = std::max(stackSizes.cssMS, ss.cssMS);
        stackSizes.cssCH = std::max(stackSizes.cssCH, ss.cssCH);
    }

    OPTIX_CHECK(optixPipelineSetStackSize(
        _pipeline,
        stackSizes.cssRG + stackSizes.cssCH,  // direct callable
        stackSizes.cssRG + stackSizes.cssCH,  // continuation callable
        stackSizes.cssMS + stackSizes.cssCH,  // miss + CH
        1));  // max traversal depth

    fprintf(stderr, "SpectralGPU: pipeline created\n");

    // SBT — raygen record
    RayGenSbtRecord raygenRec = {};
    OPTIX_CHECK(optixSbtRecordPackHeader(_raygenPG, &raygenRec));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_sbtRaygenRecord), sizeof(RayGenSbtRecord)));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_sbtRaygenRecord), &raygenRec,
                          sizeof(RayGenSbtRecord), cudaMemcpyHostToDevice));

    // SBT — miss record
    MissSbtRecord missRec = {};
    OPTIX_CHECK(optixSbtRecordPackHeader(_missPG, &missRec));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_sbtMissRecord), sizeof(MissSbtRecord)));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_sbtMissRecord), &missRec,
                          sizeof(MissSbtRecord), cudaMemcpyHostToDevice));

    // SBT struct (hitgroup populated in BuildAccel when we know normals pointer)
    _sbt.raygenRecord                = _sbtRaygenRecord;
    _sbt.missRecordBase              = _sbtMissRecord;
    _sbt.missRecordStrideInBytes     = sizeof(MissSbtRecord);
    _sbt.missRecordCount             = 1;

    // Allocate params buffer
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_params),
                          sizeof(spectral_gpu::LaunchParams)));

    fprintf(stderr, "SpectralGPU: initialization complete\n");
    return true;
}

// ---------------------------------------------------------------------------
// BuildAccel — build GAS from scene triangles
// ---------------------------------------------------------------------------
bool SpectralGPU::BuildAccel(const SpectralScene& scene)
{
    _FreeAccel();
    if (_d_normals) { cudaFree(reinterpret_cast<void*>(_d_normals)); _d_normals = 0; }

    // Flatten triangles
    std::vector<float3> vertices;
    std::vector<float3> normals;
    vertices.reserve(scene.TotalTriangles() * 3);
    normals.reserve(scene.TotalTriangles() * 3);

    for (auto& kv : scene.GetMeshes()) {
        if (!kv.second.visible) continue;
        for (auto& tri : kv.second.triangles) {
            vertices.push_back(make_float3(tri.v0[0], tri.v0[1], tri.v0[2]));
            vertices.push_back(make_float3(tri.v1[0], tri.v1[1], tri.v1[2]));
            vertices.push_back(make_float3(tri.v2[0], tri.v2[1], tri.v2[2]));
            normals.push_back(make_float3(tri.n0[0], tri.n0[1], tri.n0[2]));
            normals.push_back(make_float3(tri.n1[0], tri.n1[1], tri.n1[2]));
            normals.push_back(make_float3(tri.n2[0], tri.n2[1], tri.n2[2]));
        }
    }

    _triCount = static_cast<unsigned int>(vertices.size() / 3);
    if (_triCount == 0) {
        fprintf(stderr, "SpectralGPU: no triangles to build\n");
        return true;
    }

    // Upload vertices
    CUdeviceptr d_vertices = 0;
    const size_t vertexBytes = vertices.size() * sizeof(float3);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_vertices), vertexBytes));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(d_vertices), vertices.data(),
                          vertexBytes, cudaMemcpyHostToDevice));

    // Upload normals
    const size_t normalBytes = normals.size() * sizeof(float3);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_normals), normalBytes));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_normals), normals.data(),
                          normalBytes, cudaMemcpyHostToDevice));

    // Build indices (trivial: 0,1,2, 3,4,5, ...)
    std::vector<unsigned int> indices(_triCount * 3);
    for (unsigned int i = 0; i < _triCount * 3; ++i) indices[i] = i;

    CUdeviceptr d_indices = 0;
    const size_t indexBytes = indices.size() * sizeof(unsigned int);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_indices), indexBytes));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(d_indices), indices.data(),
                          indexBytes, cudaMemcpyHostToDevice));

    // Build input
    OptixBuildInput buildInput = {};
    buildInput.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    buildInput.triangleArray.vertexFormat        = OPTIX_VERTEX_FORMAT_FLOAT3;
    buildInput.triangleArray.vertexStrideInBytes  = sizeof(float3);
    buildInput.triangleArray.numVertices          = static_cast<unsigned int>(vertices.size());
    buildInput.triangleArray.vertexBuffers        = &d_vertices;
    buildInput.triangleArray.indexFormat           = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    buildInput.triangleArray.indexStrideInBytes    = sizeof(unsigned int) * 3;
    buildInput.triangleArray.numIndexTriplets      = _triCount;
    buildInput.triangleArray.indexBuffer            = d_indices;

    unsigned int inputFlags[] = { OPTIX_GEOMETRY_FLAG_NONE };
    buildInput.triangleArray.flags         = inputFlags;
    buildInput.triangleArray.numSbtRecords = 1;

    // Accel options
    OptixAccelBuildOptions accelOptions = {};
    accelOptions.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
    accelOptions.operation  = OPTIX_BUILD_OPERATION_BUILD;

    // Query buffer sizes
    OptixAccelBufferSizes bufferSizes;
    OPTIX_CHECK(optixAccelComputeMemoryUsage(
        _optixContext, &accelOptions, &buildInput, 1, &bufferSizes));

    CUdeviceptr d_temp = 0;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_temp), bufferSizes.tempSizeInBytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_gasBuffer), bufferSizes.outputSizeInBytes));

    // Build
    OPTIX_CHECK(optixAccelBuild(
        _optixContext, 0,  // stream
        &accelOptions, &buildInput, 1,
        d_temp, bufferSizes.tempSizeInBytes,
        _gasBuffer, bufferSizes.outputSizeInBytes,
        &_gasHandle, nullptr, 0));

    CUDA_CHECK(cudaDeviceSynchronize());

    // Free temp buffers
    cudaFree(reinterpret_cast<void*>(d_temp));
    cudaFree(reinterpret_cast<void*>(d_vertices));
    cudaFree(reinterpret_cast<void*>(d_indices));

    // Update SBT hit group with normals pointer
    if (_sbtHitgroupRecord) {
        cudaFree(reinterpret_cast<void*>(_sbtHitgroupRecord));
        _sbtHitgroupRecord = 0;
    }

    HitGroupSbtRecord hitRec = {};
    hitRec.data.normals = reinterpret_cast<float3*>(_d_normals);
    OPTIX_CHECK(optixSbtRecordPackHeader(_hitgroupPG, &hitRec));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_sbtHitgroupRecord), sizeof(HitGroupSbtRecord)));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_sbtHitgroupRecord), &hitRec,
                          sizeof(HitGroupSbtRecord), cudaMemcpyHostToDevice));

    _sbt.hitgroupRecordBase          = _sbtHitgroupRecord;
    _sbt.hitgroupRecordStrideInBytes = sizeof(HitGroupSbtRecord);
    _sbt.hitgroupRecordCount         = 1;

    fprintf(stderr, "SpectralGPU: built OptiX GAS for %u triangles\n", _triCount);
    return true;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
bool SpectralGPU::Render(const SpectralCamera& camera,
                          unsigned int width, unsigned int height,
                          float* pixels, float* depth, int spp)
{
    if (!_pipeline || !_gasHandle) return false;
    if (width == 0 || height == 0) return false;

    // (Re)allocate framebuffer if size changed
    if (width != _allocW || height != _allocH) {
        if (_d_framebuffer) cudaFree(reinterpret_cast<void*>(_d_framebuffer));
        if (_d_depthbuffer) cudaFree(reinterpret_cast<void*>(_d_depthbuffer));
        _d_framebuffer = 0;
        _d_depthbuffer = 0;

        size_t fbSize = size_t(width) * height * sizeof(float4);
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_framebuffer), fbSize));

        size_t dbSize = size_t(width) * height * sizeof(float);
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_depthbuffer), dbSize));

        _allocW = width;
        _allocH = height;
    }

    // Build camera frame vectors from the projection inverse and view-to-world
    // Extract the camera basis from the view-to-world matrix
    // PXR convention: camera looks down -Z, Y is up, X is right
    auto extract3 = [](const pxr::GfMatrix4d& m, int col) -> float3 {
        return make_float3(
            static_cast<float>(m[0][col]),
            static_cast<float>(m[1][col]),
            static_cast<float>(m[2][col]));
    };

    float3 camRight   = extract3(camera.viewToWorld, 0);
    float3 camUp      = extract3(camera.viewToWorld, 1);
    float3 camForward = extract3(camera.viewToWorld, 2);  // -Z in camera space
    float3 camOrigin  = make_float3(
        static_cast<float>(camera.viewToWorld[3][0]),
        static_cast<float>(camera.viewToWorld[3][1]),
        static_cast<float>(camera.viewToWorld[3][2]));

    // Compute half-FOV from projection inverse
    // projInverse maps NDC to view space:
    //   view.x = projInverse[0][0] * ndc.x  →  tanHalfFovX = projInverse[0][0]
    //   view.y = projInverse[1][1] * ndc.y  →  tanHalfFovY = projInverse[1][1]
    float tanHalfFovX = static_cast<float>(std::abs(camera.projInverse[0][0]));
    float tanHalfFovY = static_cast<float>(std::abs(camera.projInverse[1][1]));

    // Fill launch params
    spectral_gpu::LaunchParams launchParams = {};
    launchParams.framebuffer = reinterpret_cast<float4*>(_d_framebuffer);
    launchParams.depthbuffer = reinterpret_cast<float*>(_d_depthbuffer);
    launchParams.width       = width;
    launchParams.height      = height;
    launchParams.traversable = _gasHandle;
    launchParams.spp         = spp;
    launchParams.normals     = reinterpret_cast<float3*>(_d_normals);
    launchParams.triCount    = _triCount;

    launchParams.camera.origin       = camOrigin;
    launchParams.camera.U            = camRight;
    launchParams.camera.V            = camUp;
    launchParams.camera.W            = make_float3(-camForward.x, -camForward.y, -camForward.z);
    launchParams.camera.tanHalfFovX  = tanHalfFovX;
    launchParams.camera.tanHalfFovY  = tanHalfFovY;

    // Upload launch params
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_params), &launchParams,
                          sizeof(spectral_gpu::LaunchParams), cudaMemcpyHostToDevice));

    // Launch
    OPTIX_CHECK(optixLaunch(
        _pipeline, 0,
        _d_params, sizeof(spectral_gpu::LaunchParams),
        &_sbt,
        width, height, 1));

    CUDA_CHECK(cudaDeviceSynchronize());

    // Download framebuffer
    CUDA_CHECK(cudaMemcpy(pixels,
                          reinterpret_cast<void*>(_d_framebuffer),
                          size_t(width) * height * sizeof(float4),
                          cudaMemcpyDeviceToHost));

    // Download depth
    if (depth) {
        CUDA_CHECK(cudaMemcpy(depth,
                              reinterpret_cast<void*>(_d_depthbuffer),
                              size_t(width) * height * sizeof(float),
                              cudaMemcpyDeviceToHost));
    }

    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_OPTIX
