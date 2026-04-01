#pragma once

#include "HdSpectralApi.h"

// PXR types
#include "SpectralScene.h"
#include "SpectralIntegrator.h"

#include <pxr/usd/usd/stage.h>

#ifdef SPECTRAL_HAS_OSD
#include "SpectralSubdiv.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

// NDK headers
#include <DDImage/Iop.h>
#include <DDImage/DeepOp.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>
#include <DDImage/Format.h>
#include <DDImage/GeometryProviderI.h>
#include <DDImage/OpState.h>

#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// SpectralRenderIop
//
//   Dual Iop + DeepOp render node.
//   Flat RGBA output via engine() (Iop path).
//   Deep output via doDeepEngine() (DeepOp path).
//
//   Input 0 (Scene): GeometryProviderI — optional
//   Input 1 (Cam):   CameraOp          — optional
//   Input 2 (BG):    any Iop            — optional, sets resolution
// ---------------------------------------------------------------------------
using namespace DD::Image;

class HDSPECTRAL_API SpectralRenderIop : public Iop, public DeepOp
{
public:
    const char* Class()         const override { return CLASS; }
    const char* node_help()     const override;

    explicit SpectralRenderIop(Node* node);
    ~SpectralRenderIop() override;

    // Input 0: Scene  (GeometryProviderI) — optional
    // Input 1: Camera (CameraOp)          — optional
    // Input 2: BG     (any Iop)           — optional, sets output resolution
    int  minimum_inputs()               const override { return 0; }
    int  maximum_inputs()               const override { return 3; }
    const char* input_label(int idx, char*) const override;
    bool test_input(int idx, Op* op)    const override;
    Op*  default_input(int idx)         const override;

    void _validate(bool forReal)        override;
    void _request(int x, int y, int r, int t,
                  ChannelMask channels, int count) override;
    void engine(int y, int x, int r,
                ChannelMask channels, Row& row)    override;
    void knobs(Knob_Callback f)         override;
    int  knob_changed(Knob* k)          override;

    unsigned node_color() const override { return 0xFF8C1AFF; }

    static const Op::Description description;

    // ----- DeepOp interface -----
    const char* conversionHelperNodeClass() const override { return "DeepToImage"; }
    const Format* convertibleFormat() const override { return &(info_.format()); }
    Op* op() override { return this; }

protected:
    // DeepOp pure virtuals
    bool doDeepEngine(DD::Image::Box box,
                      const DD::Image::ChannelSet& channels,
                      DeepOutputPlane& plane) override;
    void getDeepRequests(DD::Image::Box box,
                         const DD::Image::ChannelSet& channels,
                         int count,
                         std::vector<RequestData>& reqData) override;

private:
    static const char* const CLASS;

    void _LoadStage();
    void _LoadFromPxrStage(const UsdStageRefPtr& stage);
    void _BuildCameraFromInput();
    void _EnsureFrameRendered();

    // Knobs
    const char* _usdFilePath = "";
    int   _frame      = 1;
    int   _samples    = 1;
    int   _maxBounces = 4;
    int   _tileSize   = 64;
    int   _deviceMode = 0;         // 0=cpu, 1=gpu, 2=auto
    const char* _cameraPath = "";

    FormatPair _outputFormat;

    // Scene + camera built together during _LoadStage
    std::unique_ptr<pxr::SpectralScene> _scene;
    SpectralCamera                      _camera;
    bool                                _cameraFromInput = false;

    // Frame buffer (RGBA) + depth buffer
    std::vector<float>  _frameBuffer;
    std::vector<float>  _depthBuffer;   // per-pixel depth (camera-space Z)
    unsigned int        _fbWidth  = 0;
    unsigned int        _fbHeight = 0;
    std::atomic<bool>   _frameReady { false };
    std::mutex          _renderMutex;
};
