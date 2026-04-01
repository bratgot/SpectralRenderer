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

    /// Render into host-side buffers.
    /// pixels: RGBA float, width*height*4 floats (pre-allocated)
    /// depth:  per-pixel depth, width*height floats (pre-allocated, can be null)
    bool Render(const SpectralCamera& camera,
                unsigned int width, unsigned int height,
                float* pixels, float* depth = nullptr,
                int spp = 1);

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

    // Acceleration structure
    CUdeviceptr            _gasBuffer   = 0;
    OptixTraversableHandle _gasHandle   = 0;

    // SBT
    OptixShaderBindingTable _sbt = {};
    CUdeviceptr            _sbtRaygenRecord   = 0;
    CUdeviceptr            _sbtMissRecord     = 0;
    CUdeviceptr            _sbtHitgroupRecord = 0;

    // Device buffers
    CUdeviceptr            _d_framebuffer = 0;
    CUdeviceptr            _d_depthbuffer = 0;
    CUdeviceptr            _d_normals     = 0;
    CUdeviceptr            _d_params      = 0;

    // Current allocation sizes
    unsigned int           _allocW = 0;
    unsigned int           _allocH = 0;
    unsigned int           _triCount = 0;

    void _FreeAccel();
    void _FreeBuffers();
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // SPECTRAL_HAS_OPTIX
