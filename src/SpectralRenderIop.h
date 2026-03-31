#pragma once

#include "HdSpectralApi.h"

// PXR types
#include "SpectralScene.h"
#include "SpectralIntegrator.h"

PXR_NAMESPACE_USING_DIRECTIVE

// NDK headers
#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>
#include <DDImage/Format.h>

#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// SpectralRenderIop
//
//   USD file-based render node for Nuke's 2D node graph / farm rendering.
//   Reads a .usd/.usda/.usdc file via PXR UsdStage API, traverses
//   UsdGeomMesh prims, and renders through SpectralIntegrator.
//
//   Global scope + Op::Description pattern required by Nuke 17 NDK.
// ---------------------------------------------------------------------------
using namespace DD::Image;

class HDSPECTRAL_API SpectralRenderIop : public Iop
{
public:
    const char* Class()         const override { return CLASS; }
    const char* node_help()     const override;

    explicit SpectralRenderIop(Node* node);
    ~SpectralRenderIop() override;

    int  minimum_inputs()               const override { return 0; }
    int  maximum_inputs()               const override { return 0; }

    void _validate(bool forReal)        override;
    void _request(int x, int y, int r, int t,
                  ChannelMask channels, int count) override;
    void engine(int y, int x, int r,
                ChannelMask channels, Row& row)    override;
    void knobs(Knob_Callback f)         override;
    int  knob_changed(Knob* k)          override;

    unsigned node_color() const override { return 0xFF8C1AFF; }

    static const Op::Description description;

private:
    static const char* const CLASS;

    // Opens stage, loads meshes AND camera in one pass
    void _LoadStage();
    void _EnsureFrameRendered();

    // Knobs
    const char* _usdFilePath = "";
    int   _frame      = 1;
    int   _samples    = 1;
    int   _maxBounces = 4;
    int   _tileSize   = 64;
    const char* _cameraPath = "";

    FormatPair _outputFormat;

    // Scene + camera built together during _LoadStage
    std::unique_ptr<pxr::SpectralScene> _scene;
    SpectralCamera                      _camera;

    // Frame buffer
    std::vector<float>  _frameBuffer;
    unsigned int        _fbWidth  = 0;
    unsigned int        _fbHeight = 0;
    std::atomic<bool>   _frameReady { false };
    std::mutex          _renderMutex;
};
