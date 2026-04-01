// Define STB_IMAGE_IMPLEMENTATION in exactly one translation unit
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "SpectralTexture.h"

#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

bool SpectralTexture::_LoadFromFile(const std::string& path)
{
    int w, h, c;
    // Load as float (HDR if available, LDR converted to float)
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
