#pragma once

#include "HdSpectralApi.h"

// Pull in PXR types we need
#include "SpectralScene.h"
#include "SpectralIntegrator.h"

PXR_NAMESPACE_USING_DIRECTIVE

// NDK headers
#include <DDImage/Iop.h>
#include <DDImage/GeoOp.h>
#include <DDImage/CameraOp.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>
#include <DDImage/Format.h>

#include <vector>
#include <memory>
#include <atomic>
#include <mutex>

// ---------------------------------------------------------------------------
// SpectralRenderIop
//
//   IMPORTANT: This class is defined at GLOBAL scope (not inside
//   namespace DD::Image) to match the Nuke 17 NDK plugin convention.
//   The SimpleBlur example and all other Foundry NDK samples use this
//   pattern: `using namespace DD::Image;` then class at file scope.
//
//   The Op::Description static member registers the node with Nuke's
//   plugin system when the DLL is loaded via NUKE_PATH.
// ---------------------------------------------------------------------------
using namespace DD::Image;

class HDSPECTRAL_API SpectralRenderIop : public Iop
{
public:
    const char* Class()         const override { return CLASS; }
    const char* node_help()     const override;

    explicit SpectralRenderIop(Node* node);
    ~SpectralRenderIop() override;

    // Inputs
    int  minimum_inputs()               const override { return 1; }
    int  maximum_inputs()               const override { return 2; }
    const char* input_label(int idx, char*) const override;
    bool test_input(int idx, Op* op)    const override;

    // NDK pipeline
    void _validate(bool forReal)        override;
    void _request(int x, int y, int r, int t,
                  ChannelMask channels, int count) override;
    void engine(int y, int x, int r,
                ChannelMask channels, Row& row)    override;
    void knobs(Knob_Callback f)         override;
    int  knob_changed(Knob* k)          override;

    unsigned node_color() const override { return 0xFF8C1AFF; }

    // Registration — Op::Description (NOT Iop::Description) with 2-arg ctor
    static const Op::Description description;

private:
    static const char* const CLASS;

    void            _SyncScene();
    SpectralCamera  _BuildCamera() const;
    void            _EnsureFrameRendered();

    // Knobs
    int   _samples    = 1;
    int   _maxBounces = 4;
    int   _tileSize   = 64;

    // Output format — FormatPair is what Format_knob expects in Nuke 17
    FormatPair _outputFormat;

    // Render state
    std::unique_ptr<pxr::SpectralScene> _scene;
    std::vector<float>                  _frameBuffer;
    unsigned int                        _fbWidth  = 0;
    unsigned int                        _fbHeight = 0;
    std::atomic<bool>                   _frameReady { false };
    std::mutex                          _renderMutex;
};
