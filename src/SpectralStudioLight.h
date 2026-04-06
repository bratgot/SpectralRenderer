#pragma once
// SpectralStudioLight — studio lighting node for SpectralRender
// SourceGeomOp: connects to GeoScene (USD pipeline).
// Created by Marten Blumen

#include <DDImage/SourceGeomOp.h>
#include <DDImage/Knobs.h>
#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralStudioLight : public SourceGeomOp
{
public:
    class Engine : public SourceEngine
    {
    public:
        Engine(GeomOpNode* parent) : SourceEngine(parent) {}
        void createPrims(usg::GeomSceneContext& context,
                         const usg::Path& path) override;
    };

    explicit SpectralStudioLight(Node* node);

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;
    unsigned    node_color() const override { return 0xFFCC00FF; }

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;
    void build_handles(ViewerContext* ctx) override;
    void draw_handle(ViewerContext* ctx) override;

    static Op* Build(Node* node) { return new SpectralStudioLight(node); }
    static const char* const CLASS;
    static const GeomOp::Description description;

    // --- Display ---
    bool   showLights = true;
    bool   showCones = true;
    double rigRadius = 80.0;

    // --- Preset ---
    int    presetCategory = 0;
    int    presetPhotoStudio = 0;
    int    presetPhotoField = 0;
    int    presetEquipment = 0;
    int    presetCinemaStyle = 0;
    int    presetFamousDP = 0;
    double mix = 1.0;

    // --- Key Light ---
    double keyAzimuth = 45.0;
    double keyElevation = 35.0;
    double keyIntensity = 5.0;
    float  keyColor[3] = {1.f, 0.97f, 0.92f};

    // --- Fill Light ---
    double fillRatio = 0.4;
    float  fillColor[3] = {0.85f, 0.9f, 1.f};

    // --- Rim Light ---
    double rimIntensity = 2.0;
    float  rimColor[3] = {1.f, 1.f, 1.f};

    // --- Shadow ---
    double shadowSoftness = 0.0;
};
