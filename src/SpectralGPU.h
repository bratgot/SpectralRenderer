#pragma once

// ---------------------------------------------------------------------------
// SpectralGPU
//
//   Host-side OptiX pipeline manager. Handles:
//     - OptiX context and module creation
//     - Program groups and pipeline linking
//     - GAS (Geometry Acceleration Structure) build from SpectralScene
//     - SBT (Shader Binding Table) setup
//     - Render launch and framebuffer download
//
//   Usage:
//     SpectralGPU gpu;
//     if (gpu.Initialize(ptxData)) {
//         gpu.BuildAccel(scene);
//         gpu.Render(camera, width, height, pixels, depth);
//     }
// ---------------------------------------------------------------------------

#ifdef SPECTRAL_HAS_OPTIX

#include "SpectralScene.h"
#include "SpectralVolume.h"
#include "SpectralGPUParams.h"

#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE
struct SpectralCamera;  // forward-declare, defined in SpectralIntegrator.h
PXR_NAMESPACE_CLOSE_SCOPE

#include <optix.h>
#include <optix_stubs.h>
#include <cuda.h>
#include <cuda_runtime.h>

#include <vector>
#include <string>
#include <functional>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralGPU
{
public:
    SpectralGPU();
    ~SpectralGPU();

    // Non-copyable
    SpectralGPU(const SpectralGPU&) = delete;
    SpectralGPU& operator=(const SpectralGPU&) = delete;

    /// Initialize OptiX context, compile module, build pipeline.
    /// ptxSource: the PTX string compiled from SpectralGPUKernel.cu
    bool Initialize(const std::string& ptxSource);

    /// Build the GAS from current scene geometry.
    /// Call once per frame when geometry changes.
    bool BuildAccel(const SpectralScene& scene);
    void InvalidateAccel() { _gasBuilt = false; _cachedSceneTriCount = UINT_MAX; _cachedLightChecksum = 0; _cachedMatChecksum = 0; _cachedTexChecksum = 0; _cachedGeoChecksum = 0; }

    /// Render into host-side buffers.
    /// pixels: RGBA float, width*height*4 floats (pre-allocated)
    /// depth:  per-pixel depth, width*height floats (pre-allocated, can be null)
    /// stripCallback: called after each strip renders with (y0, y1) range, null for full-frame
    using StripCallback = std::function<void(int y0, int y1)>;
    bool Render(const SpectralCamera& camera,
                unsigned int width, unsigned int height,
                float* pixels, float* depth = nullptr,
                int spp = 1, int maxBounces = 4, int colorSpace = 0,
                const SpectralVolume* const* volumes = nullptr,
                int numVolumes = 0,
                StripCallback stripCallback = nullptr,
                int numStrips = 1);

    /// Denoise the framebuffer in-place on GPU, copy result to host pixels.
    bool Denoise(unsigned int width, unsigned int height, float* pixels);

    /// Compute AO into aoOut (W*H floats, 1=open, 0=occluded).
    /// Requires prior BuildAccel. Uses the __raygen__ao entry point.
    bool ComputeAO(const SpectralCamera& camera,
                   unsigned int width, unsigned int height,
                   float* aoOut, int aoSamples, float aoRadius);

    /// Release all GPU resources.
    void Cleanup();

    /// True if Initialize succeeded.
    bool IsValid() const { return _pipeline != nullptr; }

private:
    // CUDA
    CUcontext              _cudaContext = nullptr;
    CUstream               _stream     = nullptr;

    // OptiX
    OptixDeviceContext     _optixContext = nullptr;
    OptixModule            _module      = nullptr;
    OptixPipeline          _pipeline    = nullptr;

    // Program groups
    OptixProgramGroup      _raygenPG    = nullptr;
    OptixProgramGroup      _missPG      = nullptr;
    OptixProgramGroup      _hitgroupPG  = nullptr;
    OptixProgramGroup      _raygenAOPG  = nullptr;   // AO-only raygen

    // Acceleration structure
    CUdeviceptr            _gasBuffer   = 0;
    OptixTraversableHandle _gasHandle   = 0;
    bool                   _hasMotionGAS = false; // built with 2 keyframes

    // SBT
    OptixShaderBindingTable _sbt = {};
    CUdeviceptr            _sbtRaygenRecord   = 0;
    CUdeviceptr            _sbtMissRecord     = 0;
    CUdeviceptr            _sbtHitgroupRecord = 0;

    // AO SBT (separate, different raygen, shared miss + hitgroup)
    OptixShaderBindingTable _sbtAO              = {};
    CUdeviceptr            _sbtRaygenAORecord   = 0;

    // Device buffers
    CUdeviceptr            _d_framebuffer  = 0;
    CUdeviceptr            _d_depthbuffer  = 0;
    CUdeviceptr            _d_normals      = 0;
    CUdeviceptr            _d_materialIds  = 0;
    CUdeviceptr            _d_materials    = 0;
    CUdeviceptr            _d_lights       = 0;
    CUdeviceptr            _d_uvs          = 0;
    CUdeviceptr            _d_textures     = 0;   // GPUTexture array
    std::vector<CUdeviceptr> _d_texPixels;         // per-texture pixel data
    CUdeviceptr            _d_params       = 0;
    CUdeviceptr            _d_uvTriIndex   = 0;   // UV projection lookup
    CUdeviceptr            _d_uvBaryU      = 0;
    CUdeviceptr            _d_uvBaryV      = 0;

    // AO output buffer (width*height floats, resized per render)
    CUdeviceptr            _d_aoBuffer     = 0;
    unsigned int           _aoAllocW       = 0;
    unsigned int           _aoAllocH       = 0;

    // Multi-volume device buffers (Phase 13) — CUDA 3D textures
    struct DeviceVolume {
        cudaArray_t           arr_density  = nullptr;
        cudaArray_t           arr_temp     = nullptr;
        cudaArray_t           arr_flame    = nullptr;
        cudaTextureObject_t   tex_density  = 0;
        cudaTextureObject_t   tex_temp     = 0;
        cudaTextureObject_t   tex_flame    = 0;
        // NanoVDB device buffers
        CUdeviceptr           nano_density = 0;
        CUdeviceptr           nano_temp    = 0;
        CUdeviceptr           nano_flame   = 0;
        size_t                nanoSizeDensity = 0;
        size_t                nanoSizeTemp    = 0;
        size_t                nanoSizeFlame   = 0;
        int                   cachedResX = 0, cachedResY = 0, cachedResZ = 0;
        int                   cachedTempResX = 0, cachedTempResY = 0, cachedTempResZ = 0;
        int                   cachedFlameResX = 0, cachedFlameResY = 0, cachedFlameResZ = 0;
        uint64_t              dataHash   = 0;
    };
    DeviceVolume           _d_volumes[SPECTRAL_MAX_GPU_VOLUMES];
    int                    _numDeviceVolumes = 0;

    // Async volume upload (pinned staging + dedicated stream)
    CUstream               _uploadStream   = nullptr;
    void*                  _pinnedStaging  = nullptr;
    size_t                 _pinnedSize     = 0;

    // Legacy single-volume (kept for cleanup)
    CUdeviceptr            _d_volumeDensity = 0;
    CUdeviceptr            _d_volumeTemp    = 0;
    size_t                 _volCachedSize   = 0;

    // Current allocation sizes
    unsigned int           _allocW = 0;
    unsigned int           _allocH = 0;
    unsigned int           _triCount = 0;
    bool                   _hasRealGeometry = false;
    bool                   _gasBuilt = false;
    unsigned int           _cachedSceneTriCount = UINT_MAX;
    unsigned int           _cachedLightChecksum = 0;
    unsigned int           _cachedMatChecksum = 0;
    unsigned int           _cachedTexChecksum = 0;
    unsigned int           _cachedGeoChecksum = 0;
    unsigned int           _materialCount = 0;
    unsigned int           _lightCount = 0;
    unsigned int           _textureCount = 0;

    // HDRI virtual lights + SH (extracted from dome in BuildAccel)
    struct { float dir[3]; float color[3]; float radius; } _virtualLights[8];
    int                    _numVirtualLights = 0;
    float                  _envSH[4][3] = {};  // 4 SH coeffs × RGB
    bool                   _hasEnvSH = false;
    float                  _envIntensityGPU = 1.f;
    // HDRI-as-background host state, copied into LaunchParams at launch.
    int                    _envVisibleBg = 0;
    int                    _envTexIdBg = -1;
    int                    _envWidthBg = 0;
    int                    _envHeightBg = 0;
    float                  _envRotationBg = 0.f;
    float                  _envIntensityBg = 1.f;

    // Denoiser
    OptixDenoiser          _denoiser = nullptr;
    CUdeviceptr            _d_denoiserState    = 0;
    CUdeviceptr            _d_denoiserScratch  = 0;
    size_t                 _denoiserStateSize   = 0;
    size_t                 _denoiserScratchSize = 0;
    unsigned int           _denoiserW = 0;
    unsigned int           _denoiserH = 0;

    void _FreeAccel();
    void _FreeBuffers();
    bool _SetupDenoiser(unsigned int W, unsigned int H);
    void _FreeDenoiser();
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_OPTIX
