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

    // Input 0: chain (SourceGeomOp default)
    // Input 1: HDRI texture (Iop — Read node, Constant, etc.)
    int  minimum_inputs() const override { return 2; }
    int  maximum_inputs() const override { return 2; }
    bool test_input(int input, Op* op) const override;
    const char* input_label(int input, char* buf) const override;
    Op*  default_input(int input) const override;

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;
    void build_handles(ViewerContext* ctx) override;
    void draw_handle(ViewerContext* ctx) override;

    static Op* Build(Node* node) { return new SpectralEnvLight(node); }
    static const char* const CLASS;
    static const GeomOp::Description description;

    // --- Display ---
    bool   showDome = true;
    bool   showSunArrow = true;
    bool   showCompass = true;
    double domeRadius = 100.0;
    // Guide appearance
    double guideLineWidth = 1.0;
    double guideIconScale = 1.0;
    double guideOpacity = 0.6;
    int    guideDashPattern = 1;   // 0=solid, 1=dashed, 2=dotted
    double guideSunColor[3] = {1.0, 0.85, 0.3};
    double guideDomeColor[3] = {0.3, 0.5, 0.8};
    double guideArcColor[3] = {1.0, 0.8, 0.3};

    // --- Sky Model ---
    int    skyPreset = 1;
    double sunElevation = 45.0;
    double sunAzimuth = 180.0;
    double sunIntensity = 5.0;
    double skyIntensity = 1.0;
    double sunShadowSoftness = 0.3;  // 0=hard, 1=soft

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
    double ndFilter = 0.0;     // ND filter in stops (0=none, 1=halve, 10=1/1024)
    double hdriShadowSoftness = 0.5; // 0=hard, 1=very soft

    // --- Environment Controls ---
    double envIntensity = 1.0;
    double envRotate = 0.0;
    double envDiffuse = 0.5;
    int    envMode = 1;
    int    envVirtualLights = 2;
    bool   useReSTIR = false;

    // HDRI from input pipe (populated during render if input 1 connected)
    bool   hasHdriInput = false;

    void ComputeSunPosition();
};
