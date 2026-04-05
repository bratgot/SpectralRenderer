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
    class Engine : public SourceEngine
    {
    public:
        Engine(DD::Image::GeomOpNode* parent) : SourceEngine(parent) {}
        void createPrims(usg::GeomSceneContext& context,
                         const usg::Path& path) override;
    };

    SpectralVDBRead(Node* node);
    ~SpectralVDBRead() override = default;

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;
    unsigned    node_color() const override { return 0x4CAF50FF; }

    void knobs(DD::Image::Knob_Callback f) override;
    int  knob_changed(DD::Image::Knob* k) override;

    std::shared_ptr<pxr::SpectralVolume> GetVolume();
    std::shared_ptr<pxr::SpectralVolume> GetVolumeAtFrame(int frame);
    bool HasVolume();

    static DD::Image::Op* Build(Node* node) { return new SpectralVDBRead(node); }
    static const DD::Image::GeomOp::Description description;

private:
    static const char* const CLASS;

    void _LoadVDB();
    void _LoadVDBAtFrame(int frame);
    void _DiscoverGrids();
    void _DetectFrameRange();
    std::string _resolveFramePath(int frame) const;
    int  _clampedFrame() const;
    int  _requestFrame = -999; // set by GetVolumeAtFrame

    const char* _filePath = "";
    bool   _autoSequence = false;
    int    _frameOffset = 0;
    int    _firstFrame = 1001;
    int    _lastFrame  = 1100;
    int    _beforeMode = 0;
    int    _afterMode  = 0;
    const char* _origFile = "";

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

    std::shared_ptr<pxr::SpectralVolume> _volume;
    std::string _loadedPath;
    int _lastViewportFrame = -999;
};
