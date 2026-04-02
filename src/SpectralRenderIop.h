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
    float _adaptiveThreshold = 0.05f; // adaptive sampling threshold
    bool  _progressive = true;          // progressive refinement mode
    bool  _blueNoise = true;            // R2 quasi-random sampling
    int   _aoSamples = 0;              // AO samples per pixel (0 = disabled)
    float _aoRadius  = 5.f;            // AO max ray distance
    int   _progressiveSppDone = 0;      // samples accumulated so far
    bool  _denoise = false;             // OptiX AI denoiser
    float _shutterOpen  = -0.5f;   // shutter open  (relative to frame)
    float _shutterClose =  0.5f;   // shutter close (relative to frame)
    float _lightIntensity = 1.0f;  // global light intensity multiplier
    int   _illuminant = 0;         // 0=auto, 1=D50, 2=D65, 3=A, 4=F2, 5=F11
    const char* _cameraPath = "";

    FormatPair _outputFormat;

    // Scene + camera built together during _LoadStage
    std::unique_ptr<pxr::SpectralScene> _scene;
    SpectralCamera                      _camera;
    bool                                _cameraFromInput = false;

    // Frame buffer (RGBA) + depth buffer + ID buffers
    std::vector<float>  _frameBuffer;
    std::vector<float>  _depthBuffer;   // per-pixel depth (camera-space Z)
    std::vector<float>  _objectIdBuffer; // per-pixel object ID
    std::vector<float>  _materialIdBuffer; // per-pixel material ID
    std::vector<float>  _accBuffer;      // progressive XYZ accumulation (3 per pixel)
    std::vector<int>    _accSampleCount; // samples accumulated per pixel
    std::vector<float>  _aoBuffer;       // ambient occlusion per pixel

    // Geometry AOV buffers (3 floats per pixel for vector channels)
    std::vector<float>  _normalBuffer;   // world normals (Nx, Ny, Nz)
    std::vector<float>  _posBuffer;      // world position (Px, Py, Pz)
    std::vector<float>  _uvBuffer;       // texture coords (u, v)
    std::vector<float>  _albedoBuffer;   // base color (r, g, b)
    std::vector<float>  _directBuffer;   // direct lighting (r, g, b)
    std::vector<float>  _indirectBuffer; // indirect/bounce lighting (r, g, b)
    std::vector<float>  _emissionBuffer; // emission (r, g, b)

    // Custom channels
    Channel _chanObjectId   = Chan_Black;
    Channel _chanMaterialId = Chan_Black;
    Channel _chanAO         = Chan_Black;
    // Geometry AOV channels
    Channel _chanNx = Chan_Black, _chanNy = Chan_Black, _chanNz = Chan_Black;
    Channel _chanPx = Chan_Black, _chanPy = Chan_Black, _chanPz = Chan_Black;
    Channel _chanUu = Chan_Black, _chanUv = Chan_Black;
    Channel _chanAlbedoR = Chan_Black, _chanAlbedoG = Chan_Black, _chanAlbedoB = Chan_Black;
    Channel _chanDirectR = Chan_Black, _chanDirectG = Chan_Black, _chanDirectB = Chan_Black;
    Channel _chanIndirectR = Chan_Black, _chanIndirectG = Chan_Black, _chanIndirectB = Chan_Black;
    Channel _chanEmissionR = Chan_Black, _chanEmissionG = Chan_Black, _chanEmissionB = Chan_Black;
    unsigned int        _fbWidth  = 0;
    unsigned int        _fbHeight = 0;
    std::atomic<bool>   _frameReady { false };
    std::mutex          _renderMutex;
};
