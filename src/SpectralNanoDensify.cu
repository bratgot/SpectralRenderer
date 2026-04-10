// SpectralNanoDensify.cu — GPU kernel to densify NanoVDB → float buffer
// Runs as plain CUDA (not OptiX), avoiding PTX bloat in the ray marching kernel.
// Created by Marten Blumen

#include <cuda_runtime.h>
#include <nanovdb/NanoVDB.h>
#include <nanovdb/math/SampleFromVoxels.h>

// Densify kernel: sample NanoVDB grid at uniform positions → dense float buffer
__global__ void densifyNanoVDB(
    const nanovdb::NanoGrid<float>* __restrict__ grid,
    float* __restrict__ output,
    int resX, int resY, int resZ,
    float3 bboxMin, float3 bboxMax)
{
    int ix = blockIdx.x * blockDim.x + threadIdx.x;
    int iy = blockIdx.y * blockDim.y + threadIdx.y;
    int iz = blockIdx.z * blockDim.z + threadIdx.z;
    if (ix >= resX || iy >= resY || iz >= resZ) return;

    // Uniform grid position → world space
    float u = (float(ix) + 0.5f) / float(resX);
    float v = (float(iy) + 0.5f) / float(resY);
    float w = (float(iz) + 0.5f) / float(resZ);
    float wx = bboxMin.x + u * (bboxMax.x - bboxMin.x);
    float wy = bboxMin.y + v * (bboxMax.y - bboxMin.y);
    float wz = bboxMin.z + w * (bboxMax.z - bboxMin.z);

    // World → index space via grid's transform, then trilinear sample
    auto ipos = grid->worldToIndexF(nanovdb::Vec3f(wx, wy, wz));
    auto acc = grid->getAccessor();
    float val = nanovdb::math::SampleFromVoxels<decltype(acc), 1>(acc)(ipos);

    // Layout: Z-major (iz * resY * resX + iy * resX + ix)
    output[iz * resY * resX + iy * resX + ix] = val;
}

// Host-callable wrapper
extern "C" void launchDensifyNanoVDB(
    const void* d_nanoGrid,
    float* d_output,
    int resX, int resY, int resZ,
    float bboxMinX, float bboxMinY, float bboxMinZ,
    float bboxMaxX, float bboxMaxY, float bboxMaxZ,
    cudaStream_t stream)
{
    float3 bmin = make_float3(bboxMinX, bboxMinY, bboxMinZ);
    float3 bmax = make_float3(bboxMaxX, bboxMaxY, bboxMaxZ);

    dim3 block(8, 8, 4);  // 256 threads
    dim3 grid_dim(
        (resX + block.x - 1) / block.x,
        (resY + block.y - 1) / block.y,
        (resZ + block.z - 1) / block.z);

    densifyNanoVDB<<<grid_dim, block, 0, stream>>>(
        reinterpret_cast<const nanovdb::NanoGrid<float>*>(d_nanoGrid),
        d_output, resX, resY, resZ, bmin, bmax);
}
