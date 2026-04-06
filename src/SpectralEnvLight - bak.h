#pragma once
// SpectralEnvLight — environment/sky lighting node for SpectralRender
// SourceGeomOp: connects to GeoScene (USD pipeline).
// Created by Marten Blumen

#include <DDImage/SourceGeomOp.h>
#include <DDImage/Knobs.h>
#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralEnvLight : public SourceGeomOp
{
public:
    class Engine : public SourceEngine
    {
    public:
        Engine(GeomOpNode* parent) : SourceEngine(parent) {}
        void createPrims(usg::GeomSceneContext& context,
                         const usg::Path& path) override;
    };

    explicit SpectralEnvLight(Node* node);

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override;
    unsigned    node_color() const override { return 0xFFCC00FF; }

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    static Op* Build(Node* node) { return new SpectralEnvLight(node); }
    static const char* const CLASS;
    static const GeomOp::Description description;

    // --- Sky Model ---
    int    skyPreset = 1;
    double sunElevation = 45.0;
    double sunAzimuth = 180.0;
    double sunIntensity = 5.0;
    double skyIntensity = 1.0;

    // --- Solar Position ---
    int    locationPreset = 0;
    double latitude = 51.5;
    double longitude = -0.12;
    double timeOfDay = 12.0;
    int    dayOfYear = 172;

    // --- HDRI ---
    const char* hdriFile = "";
    double hdriIntensity = 1.0;
    double hdriRotate = 0.0;

    // --- Environment Controls ---
    double envIntensity = 1.0;
    double envRotate = 0.0;
    double envDiffuse = 0.5;
    int    envMode = 1;
    int    envVirtualLights = 2;
    bool   useReSTIR = false;

    void ComputeSunPosition();
};
