#pragma once

// ---------------------------------------------------------------------------
// SpectralTexture
//
//   Loads image files via stb_image, provides bilinear sampling.
//   Supports RGBA float textures (LDR images converted to float on load).
//   Textures are stored in a shared table in SpectralScene.
// ---------------------------------------------------------------------------

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralTexture
{
public:
    SpectralTexture() = default;

    /// Load from file. Returns true on success.
    bool Load(const std::string& filePath)
    {
        _path = filePath;

        // stb_image is included in exactly one .cpp (SpectralIntegrator.cpp)
        // We call the load function declared below.
        return _LoadFromFile(filePath);
    }

    bool IsValid() const { return !_pixels.empty() && _width > 0 && _height > 0; }

    const std::string& GetPath() const { return _path; }
    int GetWidth()  const { return _width; }
    int GetHeight() const { return _height; }

    /// Sample the texture at UV coordinates with bilinear filtering.
    /// UV is in [0,1] range, wraps via repeat.
    GfVec3f Sample(const GfVec2f& uv) const
    {
        if (!IsValid()) return GfVec3f(1.f, 0.f, 1.f); // magenta = missing

        // Repeat wrap
        float u = uv[0] - std::floor(uv[0]);
        float v = uv[1] - std::floor(uv[1]);

        // Flip V — UV convention: (0,0) at bottom-left, image stored top-left
        v = 1.f - v;

        float fx = u * (_width - 1);
        float fy = v * (_height - 1);

        int x0 = std::max(0, std::min(int(fx), _width - 1));
        int y0 = std::max(0, std::min(int(fy), _height - 1));
        int x1 = std::min(x0 + 1, _width - 1);
        int y1 = std::min(y0 + 1, _height - 1);

        float dx = fx - x0;
        float dy = fy - y0;

        GfVec3f c00 = _GetPixel(x0, y0);
        GfVec3f c10 = _GetPixel(x1, y0);
        GfVec3f c01 = _GetPixel(x0, y1);
        GfVec3f c11 = _GetPixel(x1, y1);

        // Bilinear
        GfVec3f top    = c00 * (1.f - dx) + c10 * dx;
        GfVec3f bottom = c01 * (1.f - dx) + c11 * dx;
        return top * (1.f - dy) + bottom * dy;
    }

    /// Sample alpha channel
    float SampleAlpha(const GfVec2f& uv) const
    {
        if (!IsValid() || _channels < 4) return 1.f;

        float u = uv[0] - std::floor(uv[0]);
        float v = 1.f - (uv[1] - std::floor(uv[1]));

        float fx = u * (_width - 1);
        float fy = v * (_height - 1);

        int x0 = std::max(0, std::min(int(fx), _width - 1));
        int y0 = std::max(0, std::min(int(fy), _height - 1));

        size_t idx = (size_t(y0) * _width + x0) * _channels + 3;
        return (idx < _pixels.size()) ? _pixels[idx] : 1.f;
    }

    // ---- Internal loading (implemented in SpectralTexture.cpp) ----
    bool _LoadFromFile(const std::string& path);

//private: -- keep accessible for texture table management
    std::string        _path;
    std::vector<float> _pixels;   // RGBA float data, row-major top-left origin
    int                _width    = 0;
    int                _height   = 0;
    int                _channels = 0;

private:
    GfVec3f _GetPixel(int x, int y) const
    {
        size_t idx = (size_t(y) * _width + x) * _channels;
        float r = _pixels[idx];
        float g = (_channels >= 2) ? _pixels[idx + 1] : r;
        float b = (_channels >= 3) ? _pixels[idx + 2] : r;
        return GfVec3f(r, g, b);
    }
};

/// Texture ID for referencing textures in the scene table
using SpectralTextureId = int;
static constexpr SpectralTextureId kNoTexture = -1;

PXR_NAMESPACE_CLOSE_SCOPE
