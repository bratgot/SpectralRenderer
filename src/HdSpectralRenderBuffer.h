#pragma once

#include "HdSpectralApi.h"
#include <pxr/imaging/hd/renderBuffer.h>
#include <vector>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// HdSpectralRenderBuffer
//
//   CPU-side AOV buffer (RGBA float32 per pixel).
//
//   Hydra calls Allocate() when the render settings or resolution changes,
//   then the render pass maps the buffer via Map(), writes pixels, and
//   calls Unmap(). Nuke reads the contents via Map() + GetWidth/Height.
//
//   Phase 3: we'll add a GPU-side CUDA buffer and a download path here.
// ---------------------------------------------------------------------------
class HDSPECTRAL_API HdSpectralRenderBuffer final : public HdRenderBuffer
{
public:
    explicit HdSpectralRenderBuffer(SdfPath const& id);
    ~HdSpectralRenderBuffer() override;

    // -----------------------------------------------------------------------
    // HdRenderBuffer interface
    // -----------------------------------------------------------------------

    /// Allocate (or reallocate) storage.  Called when resolution changes.
    bool Allocate(GfVec3i const& dimensions,
                  HdFormat        format,
                  bool            multiSampled) override;

    unsigned int GetWidth()  const override { return _width;  }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth()  const override { return 1;       }
    HdFormat     GetFormat() const override { return _format; }
    bool         IsMultiSampled() const override { return false; }

    /// Lock the buffer and return a pointer to the first pixel.
    void* Map() override;

    /// Unlock the buffer.
    void  Unmap() override;

    /// True while the buffer is mapped.
    bool  IsMapped() const override { return _mapped; }

    /// True after Allocate(), false after _Deallocate().
    bool  IsConverged() const override { return _converged; }

    /// Mark the render as finished (no more samples coming).
    void  SetConverged(bool c) { _converged = c; }

    /// Reset pixel data to zero / background.
    void  Resolve() override;

    // -----------------------------------------------------------------------
    // Direct pixel write — used by the render pass (no lock needed since
    // Hydra's Execute is single-threaded for now).
    // -----------------------------------------------------------------------

    /// Write one RGBA float pixel at (x, y).  (0,0) = top-left.
    void WritePixel(unsigned int x, unsigned int y,
                    float r, float g, float b, float a = 1.0f);

    /// Pointer to raw float data (width * height * 4 floats, RGBA)
    const float* GetData() const { return _buffer.data(); }

protected:
    void _Deallocate() override;

private:
    unsigned int         _width     = 0;
    unsigned int         _height    = 0;
    HdFormat             _format    = HdFormatFloat32Vec4;
    std::vector<float>   _buffer;       // _width * _height * 4 floats
    bool                 _mapped    = false;
    bool                 _converged = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
