#include "HdSpectralRenderBuffer.h"
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/gf/vec3i.h>  
#include <cstring>  // memset

PXR_NAMESPACE_OPEN_SCOPE

HdSpectralRenderBuffer::HdSpectralRenderBuffer(SdfPath const& id)
    : HdRenderBuffer(id)
{
}

HdSpectralRenderBuffer::~HdSpectralRenderBuffer()
{
    _Deallocate();
}

bool
HdSpectralRenderBuffer::Allocate(GfVec3i const& dimensions,
                                  HdFormat        format,
                                  bool            /*multiSampled*/)
{
    _Deallocate();

    // GfVec3i::operator[] is not available in PXR 0.25.8 — use GetArray()
    const int* dims = dimensions.GetArray();
    if (dims[0] == 0 || dims[1] == 0) {
        return false;
    }

    _width     = static_cast<unsigned int>(dims[0]);
    _height    = static_cast<unsigned int>(dims[1]);
    _format    = format;
    _converged = false;

    // Always allocate as float RGBA regardless of requested format.
    // In Phase 2 we'll honour HdFormatFloat32Vec3 (beauty), etc.
    const size_t pixelCount = static_cast<size_t>(_width) * _height;
    _buffer.assign(pixelCount * 4, 0.0f);

    return true;
}

void
HdSpectralRenderBuffer::_Deallocate()
{
    _width  = 0;
    _height = 0;
    _buffer.clear();
    _buffer.shrink_to_fit();
    _converged = false;
}

void*
HdSpectralRenderBuffer::Map()
{
    _mapped = true;
    return _buffer.empty() ? nullptr : _buffer.data();
}

void
HdSpectralRenderBuffer::Unmap()
{
    _mapped = false;
}

void
HdSpectralRenderBuffer::Resolve()
{
    // For a simple accumulation buffer we don't need to do anything here.
    // Phase 3: this is where we'd divide by sample count for progressive rendering.
}

void
HdSpectralRenderBuffer::WritePixel(unsigned int x, unsigned int y,
                                    float r, float g, float b, float a)
{
    if (x >= _width || y >= _height) return;
    float* px = _buffer.data() + (static_cast<size_t>(y) * _width + x) * 4;
    px[0] = r;
    px[1] = g;
    px[2] = b;
    px[3] = a;
}

PXR_NAMESPACE_CLOSE_SCOPE