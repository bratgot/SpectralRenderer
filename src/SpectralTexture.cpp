// Define STB_IMAGE_IMPLEMENTATION in exactly one translation unit
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "SpectralTexture.h"

// EXR support via OpenEXR (ships with Nuke and vcpkg via OpenVDB)
#ifdef SPECTRAL_HAS_OPENEXR
#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable: 4244 4305)
#endif
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#ifdef _WIN32
#pragma warning(pop)
#endif
#endif // SPECTRAL_HAS_OPENEXR

#include <cstdio>
#include <cstring>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

#ifdef SPECTRAL_HAS_OPENEXR
static bool _LoadEXR(const std::string& path, int& w, int& h, int& c, std::vector<float>& pixels)
{
    try {
        Imf::InputFile file(path.c_str());
        auto dw = file.header().dataWindow();
        w = dw.max.x - dw.min.x + 1;
        h = dw.max.y - dw.min.y + 1;
        if (w <= 0 || h <= 0) return false;

        const Imf::ChannelList& channels = file.header().channels();
        bool hasR = channels.findChannel("R") != nullptr;
        bool hasG = channels.findChannel("G") != nullptr;
        bool hasB = channels.findChannel("B") != nullptr;
        bool hasA = channels.findChannel("A") != nullptr;
        c = hasA ? 4 : 3;

        pixels.resize(size_t(w) * h * c, 0.f);
        size_t stride = size_t(c) * sizeof(float);
        size_t xStride = stride;
        size_t yStride = size_t(w) * stride;

        Imf::FrameBuffer fb;
        char* base = reinterpret_cast<char*>(pixels.data())
                     - (dw.min.x * xStride + dw.min.y * yStride);

        if (hasR) fb.insert("R", Imf::Slice(Imf::FLOAT, base + 0*sizeof(float), xStride, yStride));
        if (hasG) fb.insert("G", Imf::Slice(Imf::FLOAT, base + 1*sizeof(float), xStride, yStride));
        if (hasB) fb.insert("B", Imf::Slice(Imf::FLOAT, base + 2*sizeof(float), xStride, yStride));
        if (hasA) fb.insert("A", Imf::Slice(Imf::FLOAT, base + 3*sizeof(float), xStride, yStride));

        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);

        fprintf(stderr, "SpectralTexture: loaded EXR '%s' (%dx%d, %d ch)\n", path.c_str(), w, h, c);
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "SpectralTexture: EXR error '%s': %s\n", path.c_str(), e.what());
        return false;
    }
}
#endif // SPECTRAL_HAS_OPENEXR

bool SpectralTexture::_LoadFromFile(const std::string& path)
{
    // Try EXR first for .exr files
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

#ifdef SPECTRAL_HAS_OPENEXR
    if (ext == "exr") {
        int w, h, c;
        std::vector<float> pixels;
        if (_LoadEXR(path, w, h, c, pixels)) {
            _width = w; _height = h; _channels = std::min(c, 3);
            // Convert to 3-channel if needed
            if (c == 4) {
                _pixels.resize(size_t(w) * h * 3);
                for (int i = 0; i < w * h; ++i) {
                    _pixels[i*3+0] = pixels[i*4+0];
                    _pixels[i*3+1] = pixels[i*4+1];
                    _pixels[i*3+2] = pixels[i*4+2];
                }
                _channels = 3;
            } else {
                _pixels = std::move(pixels);
            }
            return true;
        }
        // Fall through to stbi if EXR fails
    }
#else
    if (ext == "exr") {
        fprintf(stderr, "SpectralTexture: EXR not supported (OpenEXR not linked). Use .hdr format.\n");
        return false;
    }
#endif

    // stbi handles .hdr (Radiance), .png, .jpg, .bmp, .tga
    int w, h, c;
    float* data = stbi_loadf(path.c_str(), &w, &h, &c, 0);
    if (!data) {
        fprintf(stderr, "SpectralTexture: failed to load '%s': %s\n",
                path.c_str(), stbi_failure_reason());
        return false;
    }

    _width    = w;
    _height   = h;
    _channels = c;
    _pixels.assign(data, data + size_t(w) * h * c);

    stbi_image_free(data);

    fprintf(stderr, "SpectralTexture: loaded '%s' (%dx%d, %d channels)\n",
            path.c_str(), w, h, c);
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
