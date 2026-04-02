#pragma once

// ---------------------------------------------------------------------------
// SpectralPhotonMap
//
//   Stores spectral photons for caustic rendering.
//   Each photon carries a single wavelength and power — the spectral
//   renderer's per-wavelength transport naturally separates the rainbow.
//
//   Uses a spatial hash grid for fast nearest-photon gathering.
//   Kd-tree would be more memory efficient but grid is simpler
//   and works well for localised caustic patterns.
// ---------------------------------------------------------------------------

#include <pxr/base/gf/vec3f.h>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

struct SpectralPhoton {
    GfVec3f position;     // world-space hit point
    GfVec3f direction;    // incoming direction (toward surface)
    float   wavelength;   // nm (380-780)
    float   power;        // spectral radiance carried
};

class SpectralPhotonMap {
public:
    void Clear()
    {
        _photons.clear();
        _grid.clear();
        _built = false;
    }

    void Store(const SpectralPhoton& p)
    {
        _photons.push_back(p);
    }

    // Build spatial hash grid after all photons are stored
    void Build(float cellSize = 0.5f)
    {
        _cellSize = cellSize;
        _invCell = 1.f / cellSize;
        _grid.clear();

        for (size_t i = 0; i < _photons.size(); ++i) {
            uint64_t key = _HashPos(_photons[i].position);
            _grid[key].push_back(static_cast<int>(i));
        }
        _built = true;
    }

    size_t PhotonCount() const { return _photons.size(); }

    // Gather photons within radius of a point
    // Returns caustic spectral radiance at the given wavelength
    float GatherCaustic(
        const GfVec3f& pos,
        const GfVec3f& N,
        const GfVec3f& V,
        float gatherRadius,
        float queryLambda) const
    {
        if (!_built || _photons.empty()) return 0.f;

        float r2 = gatherRadius * gatherRadius;
        float radiance = 0.f;
        int gathered = 0;

        // Search 3x3x3 grid cells around the query point
        int cx = static_cast<int>(std::floor(pos[0] * _invCell));
        int cy = static_cast<int>(std::floor(pos[1] * _invCell));
        int cz = static_cast<int>(std::floor(pos[2] * _invCell));
        int cellRange = static_cast<int>(std::ceil(gatherRadius * _invCell)) + 1;

        for (int dz = -cellRange; dz <= cellRange; ++dz)
        for (int dy = -cellRange; dy <= cellRange; ++dy)
        for (int dx = -cellRange; dx <= cellRange; ++dx) {
            uint64_t key = _HashCell(cx+dx, cy+dy, cz+dz);
            auto it = _grid.find(key);
            if (it == _grid.end()) continue;

            for (int idx : it->second) {
                const SpectralPhoton& p = _photons[idx];

                // Distance check
                GfVec3f d = p.position - pos;
                float dist2 = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
                if (dist2 > r2) continue;

                // Wavelength proximity — only gather photons near our query wavelength
                // Spectral kernel: 10nm bandwidth Gaussian
                float dLambda = p.wavelength - queryLambda;
                float spectralWeight = std::exp(-dLambda * dLambda / (2.f * 10.f * 10.f));
                if (spectralWeight < 0.01f) continue;

                // Cosine weight: photon must arrive from above the surface
                float NdotD = -(N[0]*p.direction[0] + N[1]*p.direction[1] + N[2]*p.direction[2]);
                if (NdotD <= 0.f) continue;

                // Epanechnikov kernel for smooth falloff
                float kernelW = 1.f - dist2 / r2;

                radiance += p.power * spectralWeight * kernelW * NdotD;
                gathered++;
            }
        }

        if (gathered == 0) return 0.f;

        // Density estimation: divide by area of gather disk
        float area = 3.14159f * r2;
        return radiance / area;
    }

private:
    std::vector<SpectralPhoton> _photons;
    std::unordered_map<uint64_t, std::vector<int>> _grid;
    float _cellSize = 0.5f;
    float _invCell  = 2.f;
    bool  _built    = false;

    uint64_t _HashCell(int x, int y, int z) const
    {
        // Combine cell coordinates into a single hash key
        uint64_t hx = static_cast<uint64_t>(x * 73856093u);
        uint64_t hy = static_cast<uint64_t>(y * 19349669u);
        uint64_t hz = static_cast<uint64_t>(z * 83492791u);
        return hx ^ hy ^ hz;
    }

    uint64_t _HashPos(const GfVec3f& p) const
    {
        int cx = static_cast<int>(std::floor(p[0] * _invCell));
        int cy = static_cast<int>(std::floor(p[1] * _invCell));
        int cz = static_cast<int>(std::floor(p[2] * _invCell));
        return _HashCell(cx, cy, cz);
    }
};

PXR_NAMESPACE_CLOSE_SCOPE
