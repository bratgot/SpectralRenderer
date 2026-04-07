#ifdef SPECTRAL_HAS_OPTIX

#include "SpectralGPU.h"
#include "SpectralIntegrator.h"  // full SpectralCamera definition

#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdlib>
#include <cstdio>

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
    if (_d_framebuffer)  { cudaFree(reinterpret_cast<void*>(_d_framebuffer));  _d_framebuffer = 0; }
    if (_d_depthbuffer)  { cudaFree(reinterpret_cast<void*>(_d_depthbuffer));  _d_depthbuffer = 0; }
    if (_d_normals)      { cudaFree(reinterpret_cast<void*>(_d_normals));      _d_normals = 0; }
    if (_d_materialIds)  { cudaFree(reinterpret_cast<void*>(_d_materialIds));  _d_materialIds = 0; }
    if (_d_materials)    { cudaFree(reinterpret_cast<void*>(_d_materials));    _d_materials = 0; }
    if (_d_lights)       { cudaFree(reinterpret_cast<void*>(_d_lights));       _d_lights = 0; }
    if (_d_uvs)          { cudaFree(reinterpret_cast<void*>(_d_uvs));          _d_uvs = 0; }
    for (auto& dp : _d_texPixels) { if (dp) cudaFree(reinterpret_cast<void*>(dp)); }
    _d_texPixels.clear();
    if (_d_textures)     { cudaFree(reinterpret_cast<void*>(_d_textures));     _d_textures = 0; }
    _FreeDenoiser();
    if (_d_params)       { cudaFree(reinterpret_cast<void*>(_d_params));       _d_params = 0; }
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

    // Enable disk cache — avoids recompiling PTX on subsequent launches
    optixDeviceContextSetCacheEnabled(_optixContext, 1);
    // Cache in user's temp dir
    const char* tmpDir = getenv("TEMP");
    if (!tmpDir) tmpDir = getenv("TMP");
    if (tmpDir) {
        std::string cachePath = std::string(tmpDir) + "\\SpectralRenderer_optix_cache";
        optixDeviceContextSetCacheLocation(_optixContext, cachePath.c_str());
        optixDeviceContextSetCacheDatabaseSizes(_optixContext, 256 * 1024 * 1024, 512 * 1024 * 1024);
        fprintf(stderr, "SpectralGPU: OptiX disk cache at %s\n", cachePath.c_str());
    }

    fprintf(stderr, "SpectralGPU: OptiX context created\n");

    // Module
    OptixModuleCompileOptions moduleOptions = {};
    moduleOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    moduleOptions.optLevel         = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    moduleOptions.debugLevel       = OPTIX_COMPILE_DEBUG_LEVEL_NONE;

    OptixPipelineCompileOptions pipelineOptions = {};
    pipelineOptions.usesMotionBlur        = false;
    pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pipelineOptions.numPayloadValues      = 7;  // nx,ny,nz,t,matId,uvX,uvY
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
    linkOptions.maxTraceDepth = 8;  // bounces + shadow rays

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
    // Skip full rebuild if geometry hasn't changed (volume-only scenes)
    unsigned int newTriCount = scene.TotalTriangles();
    if (_gasBuilt && newTriCount == _cachedSceneTriCount) {
        const auto& lights = scene.GetLights();

        // Checksum lights to skip re-upload when unchanged
        unsigned int lightCheck = static_cast<unsigned int>(lights.size()) * 2654435761u;
        for (size_t i = 0; i < lights.size(); ++i) {
            union { float f; unsigned int u; } p;
            p.f = lights[i].EffectiveIntensity(); lightCheck ^= p.u * (unsigned(i)*73856093u+1u);
            p.f = lights[i].position[0]; lightCheck ^= p.u;
            p.f = lights[i].color[0]; lightCheck ^= p.u * 19349663u;
            p.f = lights[i].radius; lightCheck ^= p.u * 83492791u;
        }
        // Also checksum virtual light / SH state from dome lights
        for (const auto& L : lights) {
            if (L.type != pxr::SpectralLight::Type::Dome) continue;
            union { float f; unsigned int u; } p;
            p.f = L.envShadowSoftness; lightCheck ^= p.u * 49979693u;
            if (L.envHasSH) { p.f = L.envSH[0][0]; lightCheck ^= p.u; }
            lightCheck ^= static_cast<unsigned int>(L.envVirtualLights.size()) * 104729u;
        }

        bool lightsChanged = (lightCheck != _cachedLightChecksum);
        if (lightsChanged) {
            if (_d_lights) { cudaFree(reinterpret_cast<void*>(_d_lights)); _d_lights = 0; }
            _lightCount = 0;
            if (!lights.empty()) {
                std::vector<spectral_gpu::GPULight> gpuLights(lights.size());
                for (size_t i = 0; i < lights.size(); ++i) {
                    gpuLights[i].type      = static_cast<int>(lights[i].type);
                    gpuLights[i].position  = make_float3(lights[i].position[0], lights[i].position[1], lights[i].position[2]);
                    gpuLights[i].direction = make_float3(lights[i].direction[0], lights[i].direction[1], lights[i].direction[2]);
                    gpuLights[i].color     = make_float3(lights[i].color[0], lights[i].color[1], lights[i].color[2]);
                    gpuLights[i].intensity = lights[i].EffectiveIntensity();
                    gpuLights[i].colorTemperature = lights[i].colorTemperature;
                    gpuLights[i].useColorTemp = lights[i].enableColorTemperature ? 1 : 0;
                    gpuLights[i].radius    = lights[i].radius;
                    gpuLights[i].width     = lights[i].width;
                    gpuLights[i].height    = lights[i].height;
                    gpuLights[i].cosConeAngle = lights[i]._cosConeAngle;
                    gpuLights[i].cosPenumbra  = lights[i]._cosPenumbra;
                }
                size_t lightBytes = gpuLights.size() * sizeof(spectral_gpu::GPULight);
                CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_lights), lightBytes));
                CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_lights), gpuLights.data(),
                                      lightBytes, cudaMemcpyHostToDevice));
                _lightCount = static_cast<unsigned int>(gpuLights.size());
            }
            // Re-extract virtual lights + SH
            _numVirtualLights = 0;
            _hasEnvSH = false;
            _envIntensityGPU = 1.f;
            for (const auto& L : lights) {
                if (L.type != pxr::SpectralLight::Type::Dome) continue;
                int nVL = std::min(8, (int)L.envVirtualLights.size());
                for (int i = 0; i < nVL && _numVirtualLights < 8; ++i) {
                    auto& vl = L.envVirtualLights[i];
                    _virtualLights[_numVirtualLights].dir[0] = vl.direction[0];
                    _virtualLights[_numVirtualLights].dir[1] = vl.direction[1];
                    _virtualLights[_numVirtualLights].dir[2] = vl.direction[2];
                    _virtualLights[_numVirtualLights].color[0] = vl.color[0] * L.EffectiveIntensity();
                    _virtualLights[_numVirtualLights].color[1] = vl.color[1] * L.EffectiveIntensity();
                    _virtualLights[_numVirtualLights].color[2] = vl.color[2] * L.EffectiveIntensity();
                    _virtualLights[_numVirtualLights].radius = L.envShadowSoftness * 50.f;
                    ++_numVirtualLights;
                }
                if (L.envHasSH) {
                    for (int b = 0; b < 4; ++b) {
                        _envSH[b][0] = L.envSH[b][0] * L.EffectiveIntensity();
                        _envSH[b][1] = L.envSH[b][1] * L.EffectiveIntensity();
                        _envSH[b][2] = L.envSH[b][2] * L.EffectiveIntensity();
                    }
                    _hasEnvSH = true;
                }
                _envIntensityGPU = L.EffectiveIntensity();
            }
            _cachedLightChecksum = lightCheck;
            fprintf(stderr, "SpectralGPU: GAS cached, uploaded %u lights\n", _lightCount);
        } else {
            fprintf(stderr, "SpectralGPU: GAS + lights cached\n");
        }
        return true;
    }
    _cachedSceneTriCount = newTriCount;

    _FreeAccel();
    if (_d_normals)     { cudaFree(reinterpret_cast<void*>(_d_normals));     _d_normals = 0; }
    if (_d_materialIds) { cudaFree(reinterpret_cast<void*>(_d_materialIds)); _d_materialIds = 0; }
    if (_d_materials)   { cudaFree(reinterpret_cast<void*>(_d_materials));   _d_materials = 0; }

    // Flatten triangles
    std::vector<float3> vertices;
    std::vector<float3> normals;
    std::vector<int>    matIds;
    std::vector<float2> uvs;
    vertices.reserve(scene.TotalTriangles() * 3);
    normals.reserve(scene.TotalTriangles() * 3);
    matIds.reserve(scene.TotalTriangles());
    uvs.reserve(scene.TotalTriangles() * 3);

    for (auto& kv : scene.GetMeshes()) {
        if (!kv.second.visible) continue;
        for (auto& tri : kv.second.triangles) {
            vertices.push_back(make_float3(tri.v0[0], tri.v0[1], tri.v0[2]));
            vertices.push_back(make_float3(tri.v1[0], tri.v1[1], tri.v1[2]));
            vertices.push_back(make_float3(tri.v2[0], tri.v2[1], tri.v2[2]));
            normals.push_back(make_float3(tri.n0[0], tri.n0[1], tri.n0[2]));
            normals.push_back(make_float3(tri.n1[0], tri.n1[1], tri.n1[2]));
            normals.push_back(make_float3(tri.n2[0], tri.n2[1], tri.n2[2]));
            matIds.push_back(tri.materialId);
            uvs.push_back(make_float2(tri.uv0[0], tri.uv0[1]));
            uvs.push_back(make_float2(tri.uv1[0], tri.uv1[1]));
            uvs.push_back(make_float2(tri.uv2[0], tri.uv2[1]));
        }
    }

    _triCount = static_cast<unsigned int>(vertices.size() / 3);
    _hasRealGeometry = (_triCount > 0);
    if (_triCount == 0) {
        fprintf(stderr, "SpectralGPU: no triangles — creating dummy GAS for volume\n");
        // Add a degenerate triangle far away so GAS/traversable is valid
        vertices.push_back(make_float3(1e10f, 1e10f, 1e10f));
        vertices.push_back(make_float3(1e10f, 1e10f+0.001f, 1e10f));
        vertices.push_back(make_float3(1e10f+0.001f, 1e10f, 1e10f));
        normals.push_back(make_float3(0, 0, 1));
        normals.push_back(make_float3(0, 0, 1));
        normals.push_back(make_float3(0, 0, 1));
        matIds.push_back(0);
        uvs.push_back(make_float2(0, 0));
        uvs.push_back(make_float2(1, 0));
        uvs.push_back(make_float2(0, 1));
        _triCount = 1;
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

    // Upload material IDs (one per triangle)
    const size_t matIdBytes = matIds.size() * sizeof(int);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_materialIds), matIdBytes));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_materialIds), matIds.data(),
                          matIdBytes, cudaMemcpyHostToDevice));

    // Upload UVs (3 float2 per triangle)
    if (_d_uvs) { cudaFree(reinterpret_cast<void*>(_d_uvs)); _d_uvs = 0; }
    const size_t uvBytes = uvs.size() * sizeof(float2);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_uvs), uvBytes));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_uvs), uvs.data(),
                          uvBytes, cudaMemcpyHostToDevice));

    // Upload material table
    {
        const auto& mats = scene.GetMaterials();
        std::vector<spectral_gpu::GPUMaterial> gpuMats(mats.size());
        for (size_t i = 0; i < mats.size(); ++i) {
            gpuMats[i].baseColor    = make_float3(mats[i].baseColor[0], mats[i].baseColor[1], mats[i].baseColor[2]);
            gpuMats[i].metallic     = mats[i].metallic;
            gpuMats[i].roughness    = mats[i].roughness;
            gpuMats[i].ior          = mats[i].ior;
            gpuMats[i].opacity      = mats[i].opacity;
            gpuMats[i].emissiveColor = make_float3(mats[i].emissiveColor[0], mats[i].emissiveColor[1], mats[i].emissiveColor[2]);
            gpuMats[i].abbeNumber        = mats[i].abbeNumber;
            gpuMats[i].thinFilmThickness = mats[i].thinFilmThickness;
            gpuMats[i].baseColorTexId    = mats[i].baseColorTexId;
            gpuMats[i].textureBlend      = mats[i].textureBlend;
            gpuMats[i].bumpMapTexId      = mats[i].bumpMapTexId;
            gpuMats[i].bumpStrength      = mats[i].bumpStrength;
            gpuMats[i].absorptionColor   = make_float3(mats[i].absorptionColor[0], mats[i].absorptionColor[1], mats[i].absorptionColor[2]);
            gpuMats[i].absorptionDensity = mats[i].absorptionDensity;
            // Phase 14: diffraction + fluorescence
            gpuMats[i].gratingSpacing  = mats[i].gratingSpacing;
            gpuMats[i].gratingStrength = mats[i].gratingStrength;
            gpuMats[i].fluorAbsorb     = mats[i].fluorAbsorb;
            gpuMats[i].fluorEmit       = mats[i].fluorEmit;
            gpuMats[i].fluorStrength   = mats[i].fluorStrength;
        }
        const size_t matBytes = gpuMats.size() * sizeof(spectral_gpu::GPUMaterial);
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_materials), matBytes));
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_materials), gpuMats.data(),
                              matBytes, cudaMemcpyHostToDevice));

        fprintf(stderr, "SpectralGPU: uploaded %zu materials to device\n", gpuMats.size());
        _materialCount = static_cast<unsigned int>(gpuMats.size());
    }

    // Upload lights
    if (_d_lights) { cudaFree(reinterpret_cast<void*>(_d_lights)); _d_lights = 0; }
    _lightCount = 0;
    {
        const auto& lights = scene.GetLights();
        if (!lights.empty()) {
            std::vector<spectral_gpu::GPULight> gpuLights(lights.size());
            for (size_t i = 0; i < lights.size(); ++i) {
                gpuLights[i].type      = static_cast<int>(lights[i].type);
                gpuLights[i].position  = make_float3(lights[i].position[0], lights[i].position[1], lights[i].position[2]);
                gpuLights[i].direction = make_float3(lights[i].direction[0], lights[i].direction[1], lights[i].direction[2]);
                gpuLights[i].color     = make_float3(lights[i].color[0], lights[i].color[1], lights[i].color[2]);
                gpuLights[i].intensity = lights[i].EffectiveIntensity();
                gpuLights[i].colorTemperature = lights[i].colorTemperature;
                gpuLights[i].useColorTemp = lights[i].enableColorTemperature ? 1 : 0;
                gpuLights[i].radius    = lights[i].radius;
                gpuLights[i].width     = lights[i].width;
                gpuLights[i].height    = lights[i].height;
                gpuLights[i].cosConeAngle = lights[i]._cosConeAngle;
                gpuLights[i].cosPenumbra  = lights[i]._cosPenumbra;
            }
            size_t lightBytes = gpuLights.size() * sizeof(spectral_gpu::GPULight);
            CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_lights), lightBytes));
            CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_lights), gpuLights.data(),
                                  lightBytes, cudaMemcpyHostToDevice));
            _lightCount = static_cast<unsigned int>(gpuLights.size());
            fprintf(stderr, "SpectralGPU: uploaded %zu lights to device\n", gpuLights.size());
        }

        // Extract virtual lights + SH from dome lights for GPU volume lighting
        _numVirtualLights = 0;
        _hasEnvSH = false;
        _envIntensityGPU = 1.f;
        for (const auto& L : lights) {
            if (L.type != pxr::SpectralLight::Type::Dome) continue;

            // Virtual lights
            int nVL = std::min(8, (int)L.envVirtualLights.size());
            for (int i = 0; i < nVL && _numVirtualLights < 8; ++i) {
                auto& vl = L.envVirtualLights[i];
                _virtualLights[_numVirtualLights].dir[0] = vl.direction[0];
                _virtualLights[_numVirtualLights].dir[1] = vl.direction[1];
                _virtualLights[_numVirtualLights].dir[2] = vl.direction[2];
                _virtualLights[_numVirtualLights].color[0] = vl.color[0] * L.EffectiveIntensity();
                _virtualLights[_numVirtualLights].color[1] = vl.color[1] * L.EffectiveIntensity();
                _virtualLights[_numVirtualLights].color[2] = vl.color[2] * L.EffectiveIntensity();
                _virtualLights[_numVirtualLights].radius = L.envShadowSoftness * 50.f;
                _numVirtualLights++;
            }

            // SH coefficients
            if (L.envHasSH) {
                for (int c = 0; c < 4; ++c) {
                    _envSH[c][0] = L.envSH[c][0] * L.EffectiveIntensity();
                    _envSH[c][1] = L.envSH[c][1] * L.EffectiveIntensity();
                    _envSH[c][2] = L.envSH[c][2] * L.EffectiveIntensity();
                }
                _hasEnvSH = true;
            }
            break;  // use first dome light
        }
    }

    // Upload textures
    for (auto& dp : _d_texPixels) { if (dp) cudaFree(reinterpret_cast<void*>(dp)); }
    _d_texPixels.clear();
    if (_d_textures) { cudaFree(reinterpret_cast<void*>(_d_textures)); _d_textures = 0; }
    _textureCount = 0;
    {
        size_t numTex = scene.TextureCount();
        if (numTex > 0) {
            std::vector<spectral_gpu::GPUTexture> gpuTextures(numTex);
            _d_texPixels.resize(numTex, 0);

            for (size_t i = 0; i < numTex; ++i) {
                const auto* tex = scene.GetTexture(static_cast<int>(i));
                if (!tex || !tex->IsValid()) {
                    gpuTextures[i].pixels = nullptr;
                    gpuTextures[i].width = 0;
                    gpuTextures[i].height = 0;
                    gpuTextures[i].channels = 0;
                    continue;
                }
                size_t pixelBytes = tex->_pixels.size() * sizeof(float);
                CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_texPixels[i]), pixelBytes));
                CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_texPixels[i]),
                                      tex->_pixels.data(), pixelBytes, cudaMemcpyHostToDevice));
                gpuTextures[i].pixels = reinterpret_cast<float*>(_d_texPixels[i]);
                gpuTextures[i].width = tex->GetWidth();
                gpuTextures[i].height = tex->GetHeight();
                gpuTextures[i].channels = tex->_channels;
            }

            size_t texHdrBytes = gpuTextures.size() * sizeof(spectral_gpu::GPUTexture);
            CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_textures), texHdrBytes));
            CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_d_textures),
                                  gpuTextures.data(), texHdrBytes, cudaMemcpyHostToDevice));
            _textureCount = static_cast<unsigned int>(numTex);
            fprintf(stderr, "SpectralGPU: uploaded %zu textures to device\n", numTex);
        }
    }

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
    hitRec.data.materialIds = reinterpret_cast<int*>(_d_materialIds);
    hitRec.data.uvs = reinterpret_cast<float2*>(_d_uvs);
    OPTIX_CHECK(optixSbtRecordPackHeader(_hitgroupPG, &hitRec));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_sbtHitgroupRecord), sizeof(HitGroupSbtRecord)));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(_sbtHitgroupRecord), &hitRec,
                          sizeof(HitGroupSbtRecord), cudaMemcpyHostToDevice));

    _sbt.hitgroupRecordBase          = _sbtHitgroupRecord;
    _sbt.hitgroupRecordStrideInBytes = sizeof(HitGroupSbtRecord);
    _sbt.hitgroupRecordCount         = 1;

    fprintf(stderr, "SpectralGPU: built OptiX GAS for %u triangles\n", _triCount);
    _gasBuilt = true;
    return true;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
bool SpectralGPU::Render(const SpectralCamera& camera,
                          unsigned int width, unsigned int height,
                          float* pixels, float* depth, int spp, int maxBounces,
                          int colorSpace, const SpectralVolume* const* volumes,
                          int numVolumes)
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

    // Copy camera matrices to GPU — same ray generation as CPU _MakeRay
    // GfMatrix4d is row-major [row][col], copy as flat float[16]
    auto copyMatrix = [](const pxr::GfMatrix4d& src, float* dst) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                dst[r * 4 + c] = static_cast<float>(src[r][c]);
    };

    // Fill launch params
    spectral_gpu::LaunchParams launchParams = {};
    launchParams.framebuffer = reinterpret_cast<float4*>(_d_framebuffer);
    launchParams.depthbuffer = reinterpret_cast<float*>(_d_depthbuffer);
    launchParams.width       = width;
    launchParams.height      = height;
    launchParams.traversable = _gasHandle;
    launchParams.spp         = spp;
    launchParams.colorSpace  = colorSpace;
    launchParams.normals     = reinterpret_cast<float3*>(_d_normals);
    launchParams.materialIds = reinterpret_cast<int*>(_d_materialIds);
    launchParams.triCount    = _triCount;
    launchParams.hasRealGeometry = _hasRealGeometry ? 1 : 0;
    launchParams.materials   = reinterpret_cast<spectral_gpu::GPUMaterial*>(_d_materials);
    launchParams.materialCount = _materialCount;

    launchParams.lights     = reinterpret_cast<spectral_gpu::GPULight*>(_d_lights);
    launchParams.lightCount = _lightCount;
    launchParams.maxBounces = maxBounces;
    launchParams.uvs          = reinterpret_cast<float2*>(_d_uvs);
    launchParams.textures     = reinterpret_cast<spectral_gpu::GPUTexture*>(_d_textures);
    launchParams.textureCount = _textureCount;
    launchParams.blueNoise    = camera.blueNoise ? 1 : 0;

    // Multi-volume upload (Phase 13)
    launchParams.numGpuVolumes = 0;
    memset(launchParams.gpuVolumes, 0, sizeof(launchParams.gpuVolumes));
    // Legacy single-volume fields
    launchParams.hasVolume = 0;
    launchParams.volumeDensity = nullptr;
    launchParams.volumeTemperature = nullptr;
    launchParams.volumeFlame = nullptr;

    int nv = numVolumes;
    if (nv > SPECTRAL_MAX_GPU_VOLUMES) nv = SPECTRAL_MAX_GPU_VOLUMES;

    for (int vi = 0; vi < nv; ++vi) {
        const SpectralVolume* volume = volumes[vi];
        if (!volume || !volume->IsValid()) continue;

        int gi = launchParams.numGpuVolumes;
        spectral_gpu::GPUVolume& gv = launchParams.gpuVolumes[gi];

        gv.resX = volume->resX; gv.resY = volume->resY; gv.resZ = volume->resZ;
        gv.bboxMin = make_float3(volume->GetBboxMin()[0], volume->GetBboxMin()[1], volume->GetBboxMin()[2]);
        gv.bboxMax = make_float3(volume->GetBboxMax()[0], volume->GetBboxMax()[1], volume->GetBboxMax()[2]);
        gv.extinction = volume->extinction;
        gv.scattering = volume->scattering;
        gv.densityMult = volume->densityMult;
        gv.gForward = volume->gForward;
        gv.gBackward = volume->gBackward;
        gv.lobeMix = volume->lobeMix;
        gv.emissionIntensity = volume->emissionIntensity;
        gv.tempMin = volume->tempMin;
        gv.tempMax = volume->tempMax;
        gv.powder = volume->powderStrength;
        gv.scatterColor = make_float3(volume->scatterColor[0], volume->scatterColor[1], volume->scatterColor[2]);
        gv.stepSize = volume->stepSize;
        gv.jitter = volume->jitter ? 1 : 0;
        gv.phaseMode = volume->phaseMode;
        gv.mieDropletD = volume->mieDropletD;
        gv.shadowSteps = volume->shadowSteps;
        gv.shadowDensity = volume->shadowDensity;
        gv.quality = volume->quality;
        gv.adaptiveStep = volume->adaptiveStep ? 1 : 0;
        gv.renderMode = volume->renderMode;
        gv.intensity = volume->intensity;
        gv.flameIntensity = volume->flameIntensity;
        gv.noiseEnable = volume->noiseEnable ? 1 : 0;
        gv.noiseScale = volume->noiseScale;
        gv.noiseStrength = volume->noiseStrength;
        gv.noiseOctaves = volume->noiseOctaves;
        gv.noiseRoughness = volume->noiseRoughness;
        gv.hasTransform = volume->hasTransform ? 1 : 0;
        gv.xfCenter = make_float3(volume->_center[0], volume->_center[1], volume->_center[2]);
        gv.invScale = make_float3(volume->_invScale[0], volume->_invScale[1], volume->_invScale[2]);
        for (int i = 0; i < 9; ++i) gv.invRotM[i] = volume->_rotM[i];
        gv.origBboxMin = make_float3(volume->bboxMin[0], volume->bboxMin[1], volume->bboxMin[2]);
        gv.origBboxMax = make_float3(volume->bboxMax[0], volume->bboxMax[1], volume->bboxMax[2]);
        // Phase 14: chromatic extinction + multiple scattering
        gv.chromaticExtinction = volume->chromaticExtinction ? 1 : 0;
        gv.sigmaR = volume->sigmaR;
        gv.sigmaG = volume->sigmaG;
        gv.sigmaB = volume->sigmaB;
        gv.msApprox = volume->msApprox ? 1 : 0;
        gv.msTint = make_float3(volume->msTint[0], volume->msTint[1], volume->msTint[2]);

        // Upload density grid — skip if data unchanged (shader-only changes)
        size_t densBytes = volume->density.size() * sizeof(float);
        auto& dv = _d_volumes[gi];

        // Quick checksum: sample a few density values to detect data changes
        // (pointer changes every frame because VolMerge creates new objects)
        unsigned int dataCheck = volume->resX * 73856093u ^ volume->resY * 19349663u ^ volume->resZ * 83492791u;
        if (!volume->density.empty()) {
            size_t n = volume->density.size();
            // Sample 8 spread-out values for fast fingerprint
            for (int ci = 0; ci < 8; ++ci) {
                size_t idx = (ci * n) / 8;
                union { float f; unsigned int u; } pun;
                pun.f = volume->density[idx];
                dataCheck ^= pun.u * (ci * 2654435761u + 1u);
            }
        }
        bool gridChanged = (dv.cachedResX != volume->resX ||
                           dv.cachedResY != volume->resY ||
                           dv.cachedResZ != volume->resZ ||
                           dv.cachedChecksum != dataCheck);

        if (gridChanged) {
            if (dv.cachedSize != densBytes) {
                if (dv.d_density) cudaFree(reinterpret_cast<void*>(dv.d_density));
                dv.d_density = 0;
                CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&dv.d_density), densBytes));
                dv.cachedSize = densBytes;
            }
            CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(dv.d_density), volume->density.data(),
                                  densBytes, cudaMemcpyHostToDevice));
            dv.cachedResX = volume->resX;
            dv.cachedResY = volume->resY;
            dv.cachedResZ = volume->resZ;
            dv.cachedChecksum = dataCheck;

            // Temperature — only re-upload when grid changed
            if (dv.d_temp) { cudaFree(reinterpret_cast<void*>(dv.d_temp)); dv.d_temp = 0; }
            if (!volume->temperature.empty()) {
                size_t tempBytes = volume->temperature.size() * sizeof(float);
                CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&dv.d_temp), tempBytes));
                CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(dv.d_temp), volume->temperature.data(),
                                      tempBytes, cudaMemcpyHostToDevice));
            }

            // Flame — only re-upload when grid changed
            if (dv.d_flame) { cudaFree(reinterpret_cast<void*>(dv.d_flame)); dv.d_flame = 0; }
            if (!volume->flame.empty()) {
                size_t flameBytes = volume->flame.size() * sizeof(float);
                CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&dv.d_flame), flameBytes));
                CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(dv.d_flame), volume->flame.data(),
                                      flameBytes, cudaMemcpyHostToDevice));
            }

            fprintf(stderr, "SpectralGPU: volume[%d] uploaded %dx%dx%d (%.1f MB)\n",
                    gi, gv.resX, gv.resY, gv.resZ, densBytes / (1024.f * 1024.f));
        }
        gv.density = reinterpret_cast<float*>(dv.d_density);
        gv.temperature = dv.d_temp ? reinterpret_cast<float*>(dv.d_temp) : nullptr;
        gv.flame = dv.d_flame ? reinterpret_cast<float*>(dv.d_flame) : nullptr;

        // Also populate legacy single-volume fields for volume[0]
        if (gi == 0) {
            launchParams.hasVolume = 1;
            launchParams.volumeDensity = gv.density;
            launchParams.volumeTemperature = gv.temperature;
            launchParams.volumeFlame = gv.flame;
            launchParams.volResX = gv.resX; launchParams.volResY = gv.resY; launchParams.volResZ = gv.resZ;
            launchParams.volBboxMin = gv.bboxMin; launchParams.volBboxMax = gv.bboxMax;
            launchParams.volExtinction = gv.extinction; launchParams.volScattering = gv.scattering;
            launchParams.volDensityMult = gv.densityMult; launchParams.volGForward = gv.gForward;
            launchParams.volGBackward = gv.gBackward; launchParams.volLobeMix = gv.lobeMix;
            launchParams.volEmissionIntensity = gv.emissionIntensity;
            launchParams.volTempMin = gv.tempMin; launchParams.volTempMax = gv.tempMax;
            launchParams.volPowder = gv.powder; launchParams.volScatterColor = gv.scatterColor;
            launchParams.volStepSize = gv.stepSize; launchParams.volJitter = gv.jitter;
            launchParams.volPhaseMode = gv.phaseMode; launchParams.volMieDropletD = gv.mieDropletD;
            launchParams.volShadowSteps = gv.shadowSteps; launchParams.volShadowDensity = gv.shadowDensity;
            launchParams.volQuality = gv.quality; launchParams.volAdaptiveStep = gv.adaptiveStep;
            launchParams.volRenderMode = gv.renderMode; launchParams.volIntensity = gv.intensity;
            launchParams.volFlameIntensity = gv.flameIntensity;
            launchParams.volNoiseEnable = gv.noiseEnable; launchParams.volNoiseScale = gv.noiseScale;
            launchParams.volNoiseStrength = gv.noiseStrength; launchParams.volNoiseOctaves = gv.noiseOctaves;
            launchParams.volNoiseRoughness = gv.noiseRoughness;
            launchParams.volHasTransform = gv.hasTransform; launchParams.volXfCenter = gv.xfCenter;
            launchParams.volInvScale = gv.invScale;
            for (int i = 0; i < 9; ++i) launchParams.volInvRotM[i] = gv.invRotM[i];
            launchParams.volOrigBboxMin = gv.origBboxMin; launchParams.volOrigBboxMax = gv.origBboxMax;
        }

        launchParams.numGpuVolumes = gi + 1;
    }

    // Free unused volume slots
    for (int vi = launchParams.numGpuVolumes; vi < _numDeviceVolumes; ++vi) {
        if (_d_volumes[vi].d_density) { cudaFree(reinterpret_cast<void*>(_d_volumes[vi].d_density)); _d_volumes[vi].d_density = 0; }
        if (_d_volumes[vi].d_temp)    { cudaFree(reinterpret_cast<void*>(_d_volumes[vi].d_temp));    _d_volumes[vi].d_temp = 0; }
        if (_d_volumes[vi].d_flame)   { cudaFree(reinterpret_cast<void*>(_d_volumes[vi].d_flame));   _d_volumes[vi].d_flame = 0; }
        _d_volumes[vi].cachedSize = 0;
        _d_volumes[vi].cachedChecksum = 0;
        _d_volumes[vi].cachedResX = _d_volumes[vi].cachedResY = _d_volumes[vi].cachedResZ = 0;
    }
    _numDeviceVolumes = launchParams.numGpuVolumes;

    // Upload HDRI virtual lights + SH for GPU volume lighting
    launchParams.numVirtualLights = _numVirtualLights;
    for (int i = 0; i < _numVirtualLights; ++i) {
        launchParams.gpuVirtualLights[i].dir = make_float3(
            _virtualLights[i].dir[0], _virtualLights[i].dir[1], _virtualLights[i].dir[2]);
        launchParams.gpuVirtualLights[i].color = make_float3(
            _virtualLights[i].color[0], _virtualLights[i].color[1], _virtualLights[i].color[2]);
        launchParams.gpuVirtualLights[i].radius = _virtualLights[i].radius;
    }
    for (int i = 0; i < 4; ++i)
        launchParams.envSH[i] = make_float3(_envSH[i][0], _envSH[i][1], _envSH[i][2]);
    launchParams.hasEnvSH = _hasEnvSH ? 1 : 0;

    // projInverse: CPU uses M*v (column-vector), copy as-is
    // viewToWorld: CPU uses v*M (row-vector via Transform), so transpose for GPU
    auto copyMatrixTransposed = [](const pxr::GfMatrix4d& src, float* dst) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                dst[r * 4 + c] = static_cast<float>(src[c][r]);
    };

    copyMatrix(camera.projInverse, launchParams.camera.projInverse);
    copyMatrixTransposed(camera.viewToWorld, launchParams.camera.viewToWorld);

    // DOF params
    launchParams.camera.fStop = camera.fStop;
    launchParams.camera.focusDistance = camera.focusDistance;
    launchParams.camera.focalLength = camera.focalLength;

    // Camera basis vectors for lens disk sampling
    pxr::GfVec3d right = camera.viewToWorld.TransformDir(pxr::GfVec3d(1, 0, 0));
    pxr::GfVec3d up    = camera.viewToWorld.TransformDir(pxr::GfVec3d(0, 1, 0));
    pxr::GfVec3d fwd   = camera.viewToWorld.TransformDir(pxr::GfVec3d(0, 0, -1));
    right.Normalize(); up.Normalize(); fwd.Normalize();
    launchParams.camera.right[0]   = float(right[0]);
    launchParams.camera.right[1]   = float(right[1]);
    launchParams.camera.right[2]   = float(right[2]);
    launchParams.camera.up[0]      = float(up[0]);
    launchParams.camera.up[1]      = float(up[1]);
    launchParams.camera.up[2]      = float(up[2]);
    launchParams.camera.forward[0] = float(fwd[0]);
    launchParams.camera.forward[1] = float(fwd[1]);
    launchParams.camera.forward[2] = float(fwd[2]);

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

// ---------------------------------------------------------------------------
// Denoiser
// ---------------------------------------------------------------------------

bool SpectralGPU::_SetupDenoiser(unsigned int W, unsigned int H)
{
    if (_denoiser && _denoiserW == W && _denoiserH == H) return true;

    _FreeDenoiser();

    OptixDenoiserOptions denoiserOptions = {};
    denoiserOptions.guideAlbedo = 0;
    denoiserOptions.guideNormal = 0;

    OPTIX_CHECK(optixDenoiserCreate(
        _optixContext,
        OPTIX_DENOISER_MODEL_KIND_HDR,
        &denoiserOptions,
        &_denoiser));

    OptixDenoiserSizes sizes = {};
    OPTIX_CHECK(optixDenoiserComputeMemoryResources(
        _denoiser, W, H, &sizes));

    _denoiserStateSize   = sizes.stateSizeInBytes;
    _denoiserScratchSize = sizes.withoutOverlapScratchSizeInBytes;

    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_denoiserState), _denoiserStateSize));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&_d_denoiserScratch), _denoiserScratchSize));

    OPTIX_CHECK(optixDenoiserSetup(
        _denoiser,
        _stream,
        W, H,
        _d_denoiserState, _denoiserStateSize,
        _d_denoiserScratch, _denoiserScratchSize));

    _denoiserW = W;
    _denoiserH = H;
    fprintf(stderr, "SpectralGPU: denoiser set up for %dx%d\n", W, H);
    return true;
}

void SpectralGPU::_FreeDenoiser()
{
    if (_d_denoiserState)   { cudaFree(reinterpret_cast<void*>(_d_denoiserState));   _d_denoiserState = 0; }
    if (_d_denoiserScratch) { cudaFree(reinterpret_cast<void*>(_d_denoiserScratch)); _d_denoiserScratch = 0; }
    if (_denoiser)          { optixDenoiserDestroy(_denoiser); _denoiser = nullptr; }
    _denoiserW = _denoiserH = 0;
}

bool SpectralGPU::Denoise(unsigned int width, unsigned int height, float* pixels)
{
    if (!_optixContext) return false;

    _SetupDenoiser(width, height);
    if (!_denoiser) return false;

    size_t fbBytes = size_t(width) * height * sizeof(float4);

    // Upload host pixels to GPU input buffer
    CUdeviceptr d_input = 0;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_input), fbBytes));
    CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(d_input), pixels,
                          fbBytes, cudaMemcpyHostToDevice));

    CUdeviceptr d_output = 0;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_output), fbBytes));

    OptixImage2D inputImage = {};
    inputImage.data               = d_input;
    inputImage.width              = width;
    inputImage.height             = height;
    inputImage.rowStrideInBytes   = width * sizeof(float4);
    inputImage.pixelStrideInBytes = sizeof(float4);
    inputImage.format             = OPTIX_PIXEL_FORMAT_FLOAT4;

    OptixImage2D outputImage = inputImage;
    outputImage.data = d_output;

    OptixDenoiserGuideLayer guideLayer = {};

    OptixDenoiserLayer layer = {};
    layer.input  = inputImage;
    layer.output = outputImage;

    OptixDenoiserParams denoiserParams = {};
    denoiserParams.blendFactor = 0.f;

    OPTIX_CHECK(optixDenoiserInvoke(
        _denoiser,
        _stream,
        &denoiserParams,
        _d_denoiserState, _denoiserStateSize,
        &guideLayer,
        &layer, 1,
        0, 0,
        _d_denoiserScratch, _denoiserScratchSize));

    CUDA_CHECK(cudaStreamSynchronize(_stream));

    CUDA_CHECK(cudaMemcpy(pixels, reinterpret_cast<void*>(d_output),
                          fbBytes, cudaMemcpyDeviceToHost));

    cudaFree(reinterpret_cast<void*>(d_input));
    cudaFree(reinterpret_cast<void*>(d_output));

    fprintf(stderr, "SpectralGPU: denoised %dx%d\n", width, height);
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_OPTIX
