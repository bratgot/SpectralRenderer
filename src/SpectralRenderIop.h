#pragma once

#include "HdSpectralApi.h"

// PXR types
#include "SpectralScene.h"
#include "SpectralVolume.h"
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
#include <map>
#include <DDImage/Format.h>
#include <DDImage/ViewerContext.h>
#include <DDImage/gl.h>
#include <DDImage/Hash.h>
#include <DDImage/MetaData.h>
#include <DDImage/GeometryProviderI.h>
#include <DDImage/OpState.h>

#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// SpectralRenderIop
//
//   Dual Iop + DeepOp render node.
//   Flat RGBA output via engine() (Iop path).
//   Deep output via doDeepEngine() (DeepOp path).
//
//   Input 0 (Cam):   CameraOp          — optional (defaults to 24mm)
//   Input 1 (Scene): GeometryProviderI — optional
//   Input 2 (BG):    any Iop            — optional, sets resolution
// ---------------------------------------------------------------------------
using namespace DD::Image;

class SpectralEnvLight;
class SpectralStudioLight;
class SpectralVolumeMaterial;
class SpectralVolMerge;
class SpectralVDBRead;

class HDSPECTRAL_API SpectralRenderIop : public Iop, public DeepOp
{
public:
    const char* Class()         const override { return CLASS; }
    const char* node_help()     const override;

    explicit SpectralRenderIop(Node* node);
    ~SpectralRenderIop() override;

    // Input 0: Camera (CameraOp)          — optional (defaults to 24mm)
    // Input 1: Scene  (GeometryProviderI) — optional
    // Input 2: BG     (any Iop)           — optional, sets output resolution
    int  minimum_inputs()               const override { return 3; }
    int  maximum_inputs()               const override { return 3; }
    const char* input_label(int idx, char*) const override;
    bool test_input(int idx, Op* op)    const override;
    Op*  default_input(int idx)         const override;

    void _validate(bool forReal)        override;

    // Cryptomatte metadata for Nuke gizmo
    MetaData::Bundle _cryptoMeta;
    bool _cryptoMetaReady = false;
    const MetaData::Bundle& _fetchMetaData(const char* key) override
    {
        if (_cryptoMetaReady) return _cryptoMeta;
        return Iop::_fetchMetaData(key);
    }
    void _request(int x, int y, int r, int t,
                  ChannelMask channels, int count) override;
    void engine(int y, int x, int r,
                ChannelMask channels, Row& row)    override;
    void knobs(Knob_Callback f)         override;
    int  knob_changed(Knob* k)          override;
    void append(Hash& hash)             override;

    // 3D viewport — wireframe bbox for VDB volumes
    void build_handles(ViewerContext* ctx) override;
    void draw_handle(ViewerContext* ctx)   override;

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
    void _BuildLightRig();
    void _LoadVDB();
    void _LoadVDBForRender();
    void _applyVolumeShading(std::shared_ptr<pxr::SpectralVolume>& vol,
                              DD::Image::Op* searchFrom = nullptr);
    void _applyVolumeMaterialDirect(std::shared_ptr<pxr::SpectralVolume>& vol,
                                     SpectralVolumeMaterial* mat);
    void _EnsureFrameRendered();
    std::string _resolveFramePath(int frame) const;

    // Knobs
    const char* _usdFilePath = "";
    int   _frame      = 1;
    int   _samples    = 4;            // spectral needs 2+ (default 4)
    int   _volumeSpp  = 0;            // 0 = use _samples, else separate vol spp
    int   _maxBounces = 4;
    int   _refractionBounces = 8;  // separate limit for glass paths

    // Built-in lighting rig
    bool   _useBuiltinLight = true;  // enable built-in sun/sky when no EnvLight connected
    int    _skyPreset = 2;         // 0=off, 1=custom, 2=Clear Day, 3+ presets
    double _skyMix = 1.0;
    double _sunElevation = 45.0;
    double _sunAzimuth = 180.0;
    double _sunIntensity = 5.0;
    double _skyIntensity = 1.0;
    double _turbidity = 3.0;
    int    _locationPreset = 0;   // 0=Custom, 1+ = named locations
    double _latitude = 51.5;      // London default
    double _longitude = -0.12;
    double _timeOfDay = 12.0;     // 24h clock
    int    _dayOfYear = 172;      // June 21 (summer solstice)
    const char* _hdriFile = "";   // HDRI environment map file
    double _hdriIntensity = 1.0;
    double _hdriRotate = 0.0;

    int    _studioPreset = 0;      // 0=off, 1=portrait, 2=product, 3=dramatic
    double _studioMix = 1.0;
    double _studioKeyAzimuth = 45.0;
    double _studioKeyElevation = 35.0;
    double _studioKeyIntensity = 5.0;
    double _studioFillRatio = 0.4;
    double _studioRimIntensity = 2.0;
    double _shadowSoftness = 0.0;

    // Volume / VDB
    const char* _vdbFile = "";
    int    _vdbDensityGridIdx = 1;  // default "density"
    int    _vdbTempGridIdx = 0;
    int    _vdbFlameGridIdx = 0;
    int    _vdbColorGridIdx = 0;
    // Override fields (set by Discover Grids)
    const char* _vdbDensityOverride = "";
    const char* _vdbTempOverride = "";
    const char* _vdbFlameOverride = "";
    const char* _vdbColorOverride = "";
    static const char* const kVdbGridMenu[];
    const char* _VdbGridName(int idx, const char* ovr) const;
    int    _vdbShadingPreset = 0;
    double _vdbExtinction = 5.0;
    double _vdbScattering = 3.0;
    double _vdbDensityMult = 1.0;
    double _vdbAnisotropy = 0.0;
    double _vdbStepSize = 0.0;       // 0 = auto
    int    _vdbMaxSteps = 256;
    int    _vdbVolRes = 0;           // 0=1/8, 1=1/4, 2=1/2, 3=Full, 4=Native
    int    _scrubQuality = 1;        // 0=Fast(64³), 1=Medium(128³), 2=Full(no cap)
    const char* _vdbMemInfo = "";
    double _vdbEmissionIntensity = 2.0;
    double _vdbTempMin = 500.0;
    double _vdbTempMax = 6500.0;
    double _vdbFlameIntensity = 5.0;
    double _vdbGForward = 0.65;
    double _vdbGBackward = -0.25;
    double _vdbLobeMix = 0.70;
    double _vdbPowder = 2.0;
    double _vdbGradientMix = 0.0;
    bool   _vdbJitter = true;
    float  _vdbScatterColor[3] = {1.f, 1.f, 1.f};
    int    _vdbPhaseMode = 0;        // 0=Dual-lobe HG, 1=Approximate Mie
    double _vdbMieDropletD = 2.0;    // microns
    bool   _vdbSpectralVolumes = false; // true=spectral (slow), false=RGB (fast)

    // Shadow + quality
    int    _vdbShadowSteps = 8;
    double _vdbShadowDensity = 1.0;
    double _vdbQuality = 5.0;
    bool   _vdbAdaptiveStep = true;
    bool   _vdbMsApprox = true;
    float  _vdbMsTint[3] = {1.f, 0.97f, 0.95f};

    // Procedural detail noise
    bool   _vdbNoiseEnable = false;
    bool   _vdbNoiseNormalize = true;
    double _vdbNoiseScale = 4.0;
    double _vdbNoiseStrength = 0.3;
    int    _vdbNoiseOctaves = 3;
    double _vdbNoiseRoughness = 0.5;

    // Render mode + quality preset
    int    _vdbRenderMode = 0;       // 0=Lit,1=Greyscale,2=Heat,3=Cool,4=Blackbody,5=Explosion
    int    _vdbQualityPreset = 0;    // 0=Custom,1=Draft,2=Preview,3=Production,4=Final,5=Ultra
    double _vdbIntensity = 1.0;      // master brightness multiplier

    // Shadow cache
    bool   _vdbShadowCache = false;
    int    _vdbShadowCacheRes = 1;   // 0=Full,1=Half,2=Quarter

    // Environment map
    double _vdbEnvIntensity = 1.0;
    double _vdbEnvRotate = 0.0;
    double _vdbEnvDiffuse = 0.5;
    int    _vdbEnvMode = 1;          // 0=Uniform dirs,1=SH+Virtual Lights
    int    _vdbEnvVirtualLights = 2;
    bool   _vdbUseReSTIR = false;

    // VDB sequence
    bool   _vdbAutoSequence = false;
    int    _vdbFrameOffset = 0;
    const char* _vdbOrigFile = "";

    // Volume transform (axis controls)
    double _vdbTranslate[3] = {0, 0, 0};
    double _vdbRotate[3]    = {0, 0, 0};
    double _vdbScale[3]     = {1, 1, 1};
    double _vdbUniformScale = 1.0;

    // VDB viewport preview
    bool   _vdb3dPreview = true;   // true = SpectralRender draws its own GL preview
    bool   _vdbShowBbox = true;
    bool   _vdbShowPoints = true;
    bool   _vdbFastScrub = false;  // metadata-only during scrub
    double _vdbPointDensity = 0.3;
    double _vdbPointSize = 5.0;

    // Cached point cloud for viewport
    struct VDBPreviewPoint { float x, y, z, density, temperature, flame; };
    std::vector<VDBPreviewPoint> _vdbPreviewPoints;
    float _vdbMaxDensity = 1.f;
    float _vdbMaxTemp = 1.f;
    float _vdbMaxFlame = 1.f;
    bool  _vdbPreviewDirty = true;
    std::string _vdbLoadedPath;

    // GL volume ray march shader
    bool   _vdbShadedPreview = false;
    int    _vdbViewportRes = 2;      // 0=32, 1=64, 2=128, 3=256
    int    _vpShadowPCF = 2;        // 0=sharp, 1=3x3, 2=5x5, 3=7x7
    int    _vpVolShadowSamples = 2; // 0=off, 1=4, 2=8, 3=16
    bool   _vpEnvReflections = true;
    bool   _vpGeoReflections = true;
    bool   _vpVolReflections = true;
    int    _vpReflSteps = 1;        // 0=8, 1=16, 2=32, 3=64
    GLuint _glVolProg = 0;
    GLuint _glShadowProg = 0;
    GLuint _glGeoProg = 0;
    GLuint _glShadowDepthProg = 0;
    GLuint _glShadowFBO = 0;
    GLuint _glShadowDepthTex = 0;
    int    _glShadowMapCurSize = 0;
    static const int kShadowMapSize = 1024;
    // Reflection pre-pass FBO
    GLuint _glReflFBO = 0;
    GLuint _glReflColorTex = 0;
    GLuint _glReflDepthTex = 0;  // R32F for proper depth
    int    _glReflW = 0, _glReflH = 0;
    GLuint _glVolDensityTex = 0;
    GLuint _glVolTempTex = 0;
    int    _glVolTexFrame = -1;
    int    _glVolTexResX = 0, _glVolTexResY = 0, _glVolTexResZ = 0;
    float  _glVolMaxDensity = 0.001f;
    float  _glVolMaxTemp = 0.001f;
    void   _InitGLVolShader();
    void   _UploadGLVolTex(const pxr::SpectralVolume* vol);
    void   _DrawVolumeShaded(ViewerContext* ctx, bool writeDepth = false);

    // Scene graph transform (from GeoTransform via USD stage)
    bool  _vdbHasSceneXform = false;
    pxr::GfVec3f _vdbSceneTranslate = pxr::GfVec3f(0.f);
    pxr::GfVec3f _vdbSceneRotate = pxr::GfVec3f(0.f);
    pxr::GfVec3f _vdbSceneScale = pxr::GfVec3f(1.f);
    DD::Image::Hash _scnInputHash;  // detect upstream changes (GeoTransform etc.)
    // Bbox lock: stabilize volume position across animated frames
    bool _hasRefVolCenter = false;
    pxr::GfVec3f _refVolCenter = pxr::GfVec3f(0.f);
    bool   _vdbIsPreviewRes = false;
    bool   _vdbIsMetadataOnly = false;  // true when only bbox loaded (fast scrub)
    int    _vdbLastLoadedFrame = -999;  // cached to avoid reload

    // LRU frame cache — instant scrub-back for recently visited frames
    bool   _vdbCacheEnabled = false;
    int    _vdbCacheMax = 8;
    struct VDBCacheEntry {
        std::shared_ptr<pxr::SpectralVolume> volume;
        bool isPreviewRes;
        bool isMetadataOnly;
    };
    std::map<std::string, VDBCacheEntry> _vdbCache;
    std::vector<std::string> _vdbCacheLRU;  // most recent at back
    void _VDBCachePut(const std::string& path, std::shared_ptr<pxr::SpectralVolume> vol, bool preview, bool meta);
    VDBCacheEntry* _VDBCacheGet(const std::string& path);

    int   _tileSize   = 64;
    int   _deviceMode = 2;         // 0=cpu, 1=gpu, 2=auto
    int _GetMasterMaxRes() const;
    int   _colorSpace = 0;         // 0=sRGB, 1=ACEScg, 2=ACES 2065-1

    // LPE-style AOV decomposition
    bool  _aovDiffuseDirect  = false;
    bool  _aovSpecularDirect = false;
    bool  _aovDiffuseIndirect = false;
    bool  _aovSpecularIndirect = false;
    bool  _aovTransmission   = false;
    float _adaptiveThreshold = 0.05f; // adaptive sampling threshold
    bool  _progressive = false;          // progressive refinement mode
    bool  _blueNoise = true;            // R2 quasi-random sampling
    float _fStop = 0.f;            // 0 = pinhole (no DOF)
    float _focusDistance = 100.f;    // world units
    int   _proxyMode = 3;              // 0=1/4, 1=1/2, 2=3/4, 3=full
    bool  _caustics = false;          // enable caustic photon mapping
    int   _causticPhotons = 500000;     // photons to trace
    float _causticRadius = 0.5f;       // gather radius
    int   _aoSamples = 0;              // AO samples per pixel (0 = disabled)
    float _aoRadius  = 5.f;            // AO max ray distance
    bool  _aovNormals  = true;
    bool  _aovPosition = true;
    bool  _aovPRef    = true;
    bool  _aovUV       = true;
    bool  _aovAlbedo   = true;
    bool  _aovDirect   = false;
    bool  _aovIndirect = false;
    bool  _aovEmission = false;

    // Volume-specific AOVs
    bool  _aovVolDensity = false;
    bool  _aovVolEmission = false;
    bool  _aovVolShadow = false;
    bool  _aovVolLights = false;
    bool  _aovVolDepth = false;
    bool  _aovMotion = false;
    bool  _aovDenoiseAlbedo = false;
    bool  _aovDenoiseNormal = false;

    // Deep output
    int   _deepSamples = 0;  // 0=disabled, 16-128

    // Motion blur
    bool  _motionBlur = false;
    int   _shutterPreset = 1;  // 0=Start,1=Centre,2=End,3=Custom
    int   _motionSamples = 3;
    // Camera motion blur
    bool  _cameraMblur = false;
    int   _cameraMblurQuality = 4;  // extra samples for camera blur

    int   _progressiveSppDone = 0;      // samples accumulated so far
    bool  _denoise = false;             // OptiX AI denoiser
    // Scrub detection: skip full render during rapid frame changes
    std::chrono::steady_clock::time_point _lastFrameChangeTime;
    bool  _scrubbing = false;
    double _shutterOpen  = -0.5;   // shutter open  (relative to frame)
    double _shutterClose =  0.5;   // shutter close (relative to frame)
    float _lightIntensity = 1.0f;  // global light intensity multiplier
    int   _illuminant = 0;         // 0=auto, 1=D50, 2=D65, 3=A, 4=F2, 5=F11
    const char* _cameraPath = "";
    const char* _labelStr = "";  // node label for DAG display

    FormatPair _outputFormat;

    // Scene + camera built together during _LoadStage
    std::unique_ptr<pxr::SpectralScene> _scene;
    std::shared_ptr<pxr::SpectralVolume> _volume;  // active volume (from VDB file knob)
    std::vector<std::shared_ptr<pxr::SpectralVolume>> _volumes;  // all volumes from scn chain

    // Per-volume transforms extracted from USD stage
    struct VolumeXform {
        bool hasXform = false;
        pxr::GfVec3f translate = pxr::GfVec3f(0.f);
        pxr::GfVec3f rotate = pxr::GfVec3f(0.f);
        pxr::GfVec3f scale = pxr::GfVec3f(1.f);
    };
    std::vector<VolumeXform> _volumeXforms;

    // Cached light node pointers for 3D viewport drawing
    SpectralEnvLight*    _cachedEnvLight = nullptr;      // first, for GL preview
    SpectralStudioLight* _cachedStudioLight = nullptr;    // first, for GL preview
    SpectralVolMerge*    _cachedVolMerge = nullptr;       // for material re-application
    std::vector<SpectralVDBRead*> _cachedVdbReads;       // for render-res upgrade
    std::vector<SpectralEnvLight*>    _allEnvLights;      // all, for additive rendering
    std::vector<SpectralStudioLight*> _allStudioLights;   // all, for additive rendering
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
    std::vector<float>  _pRefBuffer;     // reference position (undisplaced)
    std::vector<float>  _uvBuffer;       // texture coords (u, v)
    std::vector<float>  _albedoBuffer;   // base color (r, g, b)
    std::vector<float>  _directBuffer;   // direct lighting (r, g, b)
    std::vector<float>  _indirectBuffer; // indirect/bounce lighting (r, g, b)
    std::vector<float>  _emissionBuffer; // emission (r, g, b)

    // LPE decomposition buffers
    std::vector<float>  _diffuseDirectBuffer;
    std::vector<float>  _specularDirectBuffer;
    std::vector<float>  _diffuseIndirectBuffer;
    std::vector<float>  _specularIndirectBuffer;
    std::vector<float>  _transmissionBuffer;

    // Cryptomatte
    std::vector<float>  _cryptoObjectBuffer;  // (hash, coverage) pairs

    // Custom channels
    Channel _chanObjectId   = Chan_Black;
    Channel _chanMaterialId = Chan_Black;
    Channel _chanAO         = Chan_Black;
    // Geometry AOV channels
    Channel _chanNx = Chan_Black, _chanNy = Chan_Black, _chanNz = Chan_Black;
    Channel _chanPx = Chan_Black, _chanPy = Chan_Black, _chanPz = Chan_Black;
    Channel _chanPRefX = Chan_Black, _chanPRefY = Chan_Black, _chanPRefZ = Chan_Black;
    Channel _chanUu = Chan_Black, _chanUv = Chan_Black;
    Channel _chanAlbedoR = Chan_Black, _chanAlbedoG = Chan_Black, _chanAlbedoB = Chan_Black;
    Channel _chanDirectR = Chan_Black, _chanDirectG = Chan_Black, _chanDirectB = Chan_Black;
    Channel _chanIndirectR = Chan_Black, _chanIndirectG = Chan_Black, _chanIndirectB = Chan_Black;
    Channel _chanEmissionR = Chan_Black, _chanEmissionG = Chan_Black, _chanEmissionB = Chan_Black;

    // LPE channels
    Channel _chanDiffDirectR = Chan_Black, _chanDiffDirectG = Chan_Black, _chanDiffDirectB = Chan_Black;
    Channel _chanSpecDirectR = Chan_Black, _chanSpecDirectG = Chan_Black, _chanSpecDirectB = Chan_Black;
    Channel _chanDiffIndirectR = Chan_Black, _chanDiffIndirectG = Chan_Black, _chanDiffIndirectB = Chan_Black;
    Channel _chanSpecIndirectR = Chan_Black, _chanSpecIndirectG = Chan_Black, _chanSpecIndirectB = Chan_Black;
    Channel _chanTransmitR = Chan_Black, _chanTransmitG = Chan_Black, _chanTransmitB = Chan_Black;

    // Cryptomatte
    Channel _chanCryptoR = Chan_Black, _chanCryptoG = Chan_Black;
    Channel _chanCryptoB = Chan_Black, _chanCryptoA = Chan_Black;
    unsigned int        _fbWidth  = 0;
    unsigned int        _fbHeight = 0;
    unsigned int        _fbFullWidth  = 0;
    unsigned int        _fbFullHeight = 0;
    std::atomic<bool>   _frameReady { false };
    std::mutex          _renderMutex;
    std::thread         _asyncQualityThread;
    std::atomic<bool>   _asyncCancel { false };
};
