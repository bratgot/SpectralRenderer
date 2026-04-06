#pragma once
// SpectralVDBRead — OpenVDB volume reader for Nuke 17
// SourceGeomOp: creates USD prims, connects to GeoScene.
// Created by Marten Blumen

#include "HdSpectralApi.h"
#include "SpectralVolume.h"

#ifdef SPECTRAL_HAS_VDB
#include "SpectralVDBLoader.h"
#endif

#include <DDImage/Knobs.h>
#include <DDImage/SourceGeomOp.h>
#include <usg/geom/GeomTokens.h>

#include <memory>
#include <string>
#include <vector>

class SpectralVDBRead : public DD::Image::SourceGeomOp
{
public:
    static constexpr const char* kAttrIsVolume    = "spectralIsVolume";
    static constexpr const char* kAttrVdbFilePath = "spectralVdbFilePath";

    class Engine : public SourceEngine
    {
    public:
        Engine(DD::Image::GeomOpNode* parent) : SourceEngine(parent) {}
        void createPrims(usg::GeomSceneContext& context,
                         const usg::Path& path) override;
    private:
        static std::string _resolveFrame(const std::string& pattern, int frame);
    };

    SpectralVDBRead(Node* node);
    ~SpectralVDBRead() override = default;

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;
    unsigned    node_color() const override { return 0x4CAF50FF; }

    // Material input (input 0) for SpectralVolumeMaterial connection
    int  minimum_inputs() const override { return 1; }
    int  maximum_inputs() const override { return 1; }
    bool test_input(int input, Op* op) const override;
    const char* input_label(int input, char* buf) const override;

    void knobs(DD::Image::Knob_Callback f) override;
    int  knob_changed(DD::Image::Knob* k) override;

    std::shared_ptr<pxr::SpectralVolume> GetVolume();
    std::shared_ptr<pxr::SpectralVolume> GetVolumeAtFrame(int frame, int maxRes = 128);
    bool HasVolume();
    int GetMaxRes() const;
    std::string ResolvePathAtFrame(int frame) const { return _resolveFramePath(frame); }

    static DD::Image::Op* Build(Node* node) { return new SpectralVDBRead(node); }
    static const DD::Image::GeomOp::Description description;

private:
    static const char* const CLASS;

    void _LoadVDB();
    void _LoadVDBAtFrame(int frame, int maxRes = 128);
    void _DiscoverGrids();
    void _DetectFrameRange();
    void _UpdateVDBInfo();
    std::string _resolveFramePath(int frame) const;
    int  _clampedFrame() const;
    int  _requestFrame = -999;

    // File
    const char* _filePath = "";
    bool   _autoSequence = false;
    int    _frameOffset = 0;
    int    _firstFrame = 1001;
    int    _lastFrame  = 1100;
    int    _beforeMode = 0;
    int    _afterMode  = 0;
    const char* _origFile = "";

    // Grids
    int _densityGridIdx = 1;
    int _tempGridIdx = 0;
    int _flameGridIdx = 0;
    int _velGridIdx = 0;
    int _colorGridIdx = 0;
    static const char* const kGridMenu[];
    const char* _densityOverride = "";
    const char* _tempOverride = "";
    const char* _flameOverride = "";
    const char* _velOverride = "";
    const char* _colorOverride = "";
    const char* _GetDensityName() const;
    const char* _GetTempName() const;

    // Volume data
    std::shared_ptr<pxr::SpectralVolume> _volume;
    std::string _loadedPath;
    int _loadedMaxRes = 0;
    int _lastViewportFrame = -999;

    // Preview (Display tab)
    bool  _showBbox    = true;
    float _bboxColor[3] = {0.0f, 0.8f, 0.2f};
    int   _previewRes  = 32;
    int   _maxPoints   = 5000;
    float _pointSize   = 2.0f;
    float _densityThreshold = 0.01f;
    bool  _lit         = false;
    // Voxel Resolution
    int   _voxelRes    = 3;  // 0=1/8, 1=1/4, 2=1/2, 3=Full, 4=Native
    const char* _memInfo = "";
    const char* _vdbInfo = "";
};
