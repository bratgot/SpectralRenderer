#pragma once

// ---------------------------------------------------------------------------
// SpectralMeshPropertiesOp -- per-mesh render attribute overrides (GeomOp)
//
// Inline geometry modifier: GeoCard -> SpectralMeshProperties -> GeoScene
// Controls render-time subdivision, normals, display colour, USD attributes.
// ---------------------------------------------------------------------------

#include <DDImage/GeomOp.h>
#include <DDImage/Knobs.h>
#include <unordered_map>
#include <string>
#include <vector>

#include "HdSpectralApi.h"

using namespace DD::Image;

class HDSPECTRAL_API SpectralMeshPropertiesOp : public GeomOp
{
public:
    static const GeomOp::Description description;
    const char* Class() const override { return description.name; }
    const char* node_help() const override;

    // Engine -- passes through geometry, registers params
    class HDSPECTRAL_API Engine : public GeomOpEngine
    {
    public:
        Engine(GeomOpNode* parent) : GeomOpEngine(parent) {}
        void processScenegraph(usg::GeomSceneContext& context) override;
    };

    SpectralMeshPropertiesOp(Node* node);
    ~SpectralMeshPropertiesOp() override;

    int minimum_inputs() const override { return 1; }
    int maximum_inputs() const override { return 1; }

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;
    void _validate(bool forReal) override;

    unsigned node_color() const override { return 0x668899ff; }

    // Registry for SpectralRenderIop to read per-mesh overrides
    struct MeshProps {
        int   subdivLevel = 0;
        int   subdivScheme = 0;
        int   normalMode = 0;
        bool  flipNormals = false;
        float displayColor[3] = {0.8f, 0.8f, 0.8f};
        bool  useDisplayColor = false;
        float displayOpacity = 1.0f;
        bool  doubleSided = true;
        int   orientation = 0;
        int   purpose = 0;
        bool  visible = true;
        bool  castsShadows = true;
        bool  receivesShadows = true;

        // Names of upstream geometry-producing Nuke nodes whose output
        // prims this MeshProperties should target. Populated in
        // RegisterParams() by walking the input(0) chain upstream and
        // collecting Op::node_name() of geometry-producer Ops (GeoCard,
        // GeoCube, etc.) that flow through this node.
        //
        // SpectralRenderIop matches a USD mesh prim path against this
        // list by checking whether the path's last component equals
        // any entry here. E.g. targetPrimPaths contains "GeoCard1" ->
        // mesh "/GeoCard1" matches.
        //
        // Empty vector means "match everything" (backwards-compat with
        // scenes that pre-date prim-path registration).
        std::vector<std::string> targetPrimPaths;
    };
    static std::unordered_map<std::string, MeshProps>& GetRegistry();
    void RegisterParams();

private:
    // Subdivision
    int   _subdivLevel = 0;
    int   _subdivScheme = 0;

    // Normals
    int   _normalMode = 0;
    bool  _flipNormals = false;

    // Display
    float _displayColor[3] = {0.8f, 0.8f, 0.8f};
    bool  _useDisplayColor = false;
    float _displayOpacity = 1.0f;

    // USD
    bool  _doubleSided = true;
    int   _orientation = 0;
    int   _purpose = 0;
    bool  _visible = true;
    bool  _castsShadows = true;
    bool  _receivesShadows = true;
};
