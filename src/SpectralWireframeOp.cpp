// ---------------------------------------------------------------------------
// SpectralWireframeOp.cpp -- artistic wireframe material
// ---------------------------------------------------------------------------

#include "SpectralWireframeOp.h"
#include <DDImage/Knobs.h>
#include "usg/geom/ShaderDesc.h"
#include "usg/base/Value.h"

using namespace DD::Image;

static const char* const kWireStyleNames[] = {
    "solid", "guide", "architectural", "hidden-line",
    "pencil sketch", "topographic", nullptr
};

static const char* const kTopoDirectionNames[] = {
    "world Y (up)", "custom direction", "normal curvature", "barycentric", nullptr
};

const char* const SpectralWireframeOp::CLASS = "SpectralWireframe";

static Op* build(Node* node) { return new SpectralWireframeOp(node); }
const Op::Description SpectralWireframeOp::description(CLASS, build);

SpectralWireframeOp::SpectralWireframeOp(Node* node)
    : ShaderOp(node)
{
}

const char* SpectralWireframeOp::node_help() const
{
    return
        "SpectralWireframe -- artistic wireframe overlay material.\n\n"
        "Renders a UV-grid wireframe over geometry, producing clean\n"
        "quad lines on subdivided meshes.\n\n"
        "Connect via GeoBindMaterial to assign to geometry.";
}

void SpectralWireframeOp::knobs(Knob_Callback f)
{
    ShaderOp::knobs(f);

    Text_knob(f,
        "<b><font size='+1'>SpectralWireframe</font></b><br>"
        "<font color='#999'>Artistic wireframe overlay for SpectralRenderer.<br>"
        "Connect via GeoBindMaterial to assign to geometry.</font>"
    );
    Divider(f);

    Enumeration_knob(f, &_wireStyle, kWireStyleNames, "wire_style", "style");
    Tooltip(f, "solid -- clean continuous lines\n"
               "guide -- thin dashed construction guidelines\n"
               "architectural -- blueprint drafting with 3 line weights\n"
               "hidden-line -- backfacing edges dashed and dimmed\n"
               "pencil sketch -- hand-drawn with wobble and cross-hatching\n"
               "topographic -- contour lines from surface elevation/curvature");

    // ===============================================================
    BeginGroup(f, "wireframe_common_grp", "Line");
    {
        Color_knob(f, _wireColor, "wire_color", "color");
        Tooltip(f, "Primary line colour.\n"
                   "White for wireframe-only renders.\n"
                   "Black or dark blue over lit surfaces.");
        Float_knob(f, &_wireThickness, "wire_thickness", "thickness");
        SetRange(f, 0.1f, 10.0f);
        Tooltip(f, "Base line width in pixels.\n"
                   "1 = hairline, 2 = medium, 3+ = bold.\n"
                   "Architectural/pencil styles vary this per-line.");
        Float_knob(f, &_wireOpacity, "wire_opacity", "opacity");
        SetRange(f, 0.0f, 1.0f);
        ClearFlags(f, Knob::STARTLINE);
        Float_knob(f, &_gridDensity, "grid_density", "grid density");
        SetRange(f, 1.0f, 64.0f);
        Tooltip(f, "Grid lines per UV unit.\n"
                   "1 = sparse, 10 = default, 32+ = dense.\n"
                   "On subdivided meshes this shows quad edges.");
        Divider(f);
        Bool_knob(f, &_wireDashed, "wire_dashed", "dashed");
        Tooltip(f, "Force dashed lines (solid/hidden-line styles).\n"
                   "Guide style is always dashed.");
        Float_knob(f, &_wireDashLength, "wire_dash_length", "dash");
        SetRange(f, 1.0f, 32.0f);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Dash length in pixels.");
        Float_knob(f, &_wireGapLength, "wire_gap_length", "gap");
        SetRange(f, 1.0f, 32.0f);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Gap length in pixels.");
    }
    EndGroup(f);

    // ===============================================================
    BeginGroup(f, "wireframe_arch_grp", "Architectural");
    {
        Text_knob(f,
            "<font color='#666' size='-1'>"
            "Blueprint-style drafting with silhouette, structural,<br>"
            "and subdivision line weights. Set style to 'architectural'."
            "</font>"
        );
        Divider(f);
        Float_knob(f, &_archSilhouetteWeight, "arch_silhouette_weight", "silhouette weight");
        SetRange(f, 1.0f, 8.0f);
        Tooltip(f, "Thickness multiplier for silhouette edges.\n"
                   "Integer UV boundaries -- like outer walls on a floor plan.\n"
                   "Default 3x. Try 4-6 for bolder outlines.");
        Float_knob(f, &_archMediumWeight, "arch_medium_weight", "structural weight");
        SetRange(f, 0.5f, 4.0f);
        Tooltip(f, "Thickness for structural edges (every 5th grid line).\n"
                   "Like interior walls. Default 1.5x.");
        Float_knob(f, &_archThinWeight, "arch_thin_weight", "thin weight");
        SetRange(f, 0.1f, 2.0f);
        Tooltip(f, "Thickness for fine subdivision lines.\n"
                   "Default 0.6x.");
        Divider(f);
        Color_knob(f, _archSilhouetteColor, "arch_silhouette_color", "silhouette color");
        Tooltip(f, "Separate colour for silhouette lines.\n"
                   "Dark red on white -> classic blueprint.\n"
                   "Black = same as main wire colour.");
        Float_knob(f, &_archThinOpacity, "arch_thin_opacity", "thin opacity");
        SetRange(f, 0.0f, 1.0f);
        Tooltip(f, "Opacity for thin subdivision lines.\n"
                   "0.4 = subtle, 1.0 = full strength.");
    }
    EndGroup(f);

    // ===============================================================
    BeginGroup(f, "wireframe_pencil_grp", "Pencil Sketch");
    {
        Text_knob(f,
            "<font color='#666' size='-1'>"
            "Hand-drawn aesthetic with natural imperfections.<br>"
            "Set style to 'pencil sketch' to activate."
            "</font>"
        );
        Divider(f);
        Float_knob(f, &_pencilWobble, "pencil_wobble", "wobble");
        SetRange(f, 0.0f, 1.0f);
        Tooltip(f, "Line wobble (hand tremor).\n"
                   "0 = perfectly straight\n"
                   "0.2-0.4 = natural sketch\n"
                   "1.0 = very shaky");
        Float_knob(f, &_pencilPressure, "pencil_pressure", "pressure");
        SetRange(f, 0.0f, 1.0f);
        Tooltip(f, "Thickness variation from pencil pressure.\n"
                   "0 = uniform thickness\n"
                   "0.5-0.7 = natural variation\n"
                   "1.0 = extreme thin-to-thick");
        Divider(f);
        Bool_knob(f, &_pencilCrossHatch, "pencil_crosshatch", "cross-hatch");
        Tooltip(f, "Add diagonal hatching in shadowed areas.\n"
                   "Uses surface normals to detect dark regions.");
        Float_knob(f, &_pencilHatchDensity, "pencil_hatch_density", "spacing");
        SetRange(f, 2.0f, 16.0f);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Pixels between hatch lines.\n"
                   "4 = dense, 16 = sparse.");
        Float_knob(f, &_pencilHatchAngle, "pencil_hatch_angle", "angle");
        SetRange(f, 0.0f, 90.0f);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Hatch angle in degrees.\n"
                   "45 = diagonal (classic)\n"
                   "0 = horizontal, 90 = vertical");
    }
    EndGroup(f);

    // ===============================================================
    BeginGroup(f, "wireframe_topo_grp", "Topographic");
    {
        Text_knob(f,
            "<font color='#666' size='-1'>"
            "Contour lines like a terrain map. Major contours are<br>"
            "drawn thicker. Set style to 'topographic' to activate."
            "</font>"
        );
        Divider(f);
        Enumeration_knob(f, &_topoDirection, kTopoDirectionNames, "topo_direction", "direction");
        Tooltip(f, "What drives the contour elevation:\n\n"
                   "world Y -- horizontal contours (terrain/landscape)\n"
                   "custom direction -- contours along any axis\n"
                   "normal curvature -- follows surface bending (sculpt/organic)\n"
                   "barycentric -- per-triangle topology (mesh QC)");
        Float_knob(f, &_topoUpVector[0], "topo_up_x", "direction");
        SetRange(f, -1.0f, 1.0f);
        Tooltip(f, "Custom direction vector (only for 'custom direction').\n"
                   "(0,1,0) = up, (1,0,0) = X slices, (0,0,1) = depth.");
        Float_knob(f, &_topoUpVector[1], "topo_up_y", "");
        SetRange(f, -1.0f, 1.0f);
        ClearFlags(f, Knob::STARTLINE);
        Float_knob(f, &_topoUpVector[2], "topo_up_z", "");
        SetRange(f, -1.0f, 1.0f);
        ClearFlags(f, Knob::STARTLINE);
        Divider(f);
        Float_knob(f, &_topoContourInterval, "topo_interval", "interval");
        SetRange(f, 0.01f, 1.0f);
        Tooltip(f, "Spacing between contour lines.\n"
                   "Smaller = more contour lines.\n"
                   "0.1 = dense, 0.5 = default, 1.0 = sparse.");
        Int_knob(f, &_topoMajorEvery, "topo_major_every", "major every");
        SetRange(f, 2, 20);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Every Nth contour is major (thicker).\n"
                   "5 = every 5th line is thick (like index contours).");
    }
    EndGroup(f);
}

int SpectralWireframeOp::knob_changed(Knob* k)
{
    RegisterParams();
    return ShaderOp::knob_changed(k);
}

std::unordered_map<std::string, SpectralWireframeOp::WireframeParams>&
SpectralWireframeOp::GetRegistry()
{
    static std::unordered_map<std::string, WireframeParams> s_registry;
    return s_registry;
}

void SpectralWireframeOp::RegisterParams()
{
    WireframeParams p;
    p.thickness     = _wireThickness;
    p.opacity       = _wireOpacity;
    p.color[0]      = _wireColor[0];
    p.color[1]      = _wireColor[1];
    p.color[2]      = _wireColor[2];
    p.dashed        = _wireDashed;
    p.dashLength    = _wireDashLength;
    p.gapLength     = _wireGapLength;
    p.nth           = _wireNth;
    p.style         = _wireStyle;
    p.gridDensity   = _gridDensity;
    p.showTriangles = _showTriangles;
    // Architectural
    p.archSilhouetteWeight = _archSilhouetteWeight;
    p.archMediumWeight     = _archMediumWeight;
    p.archThinWeight       = _archThinWeight;
    p.archSilhouetteColor[0] = _archSilhouetteColor[0];
    p.archSilhouetteColor[1] = _archSilhouetteColor[1];
    p.archSilhouetteColor[2] = _archSilhouetteColor[2];
    p.archThinOpacity      = _archThinOpacity;
    // Pencil
    p.pencilWobble       = _pencilWobble;
    p.pencilPressure     = _pencilPressure;
    p.pencilCrossHatch   = _pencilCrossHatch;
    p.pencilHatchDensity = _pencilHatchDensity;
    p.pencilHatchAngle   = _pencilHatchAngle;
    // Topo
    p.topoDirection        = _topoDirection;
    p.topoUpVector[0]      = _topoUpVector[0];
    p.topoUpVector[1]      = _topoUpVector[1];
    p.topoUpVector[2]      = _topoUpVector[2];
    p.topoContourInterval  = _topoContourInterval;
    p.topoMajorEvery       = _topoMajorEvery;
    GetRegistry()[node_name()] = p;
}

void SpectralWireframeOp::_SetShaderProperties(usg::ShaderDesc& desc,
                                                const MaterialContext& /*rtx*/)
{
    desc.overrideInput("opacity", usg::Value(0.02f));
    desc.overrideInput("roughness", usg::Value(1.0f));
}

usg::ShaderDesc* SpectralWireframeOp::createShaderGraph(
    int32_t                outputType,
    const MaterialContext& rtx,
    usg::ShaderDescGroup&  shaderGroup)
{
    std::string shaderName = getShaderNodeName(rtx.shaderFamily);
    usg::ShaderDesc* existing = shaderGroup.getShaderNode(shaderName);
    if (existing) return existing;

    usg::ShaderDesc* desc = usg::ShaderDesc::createFromSchema("UsdPreviewSurface", shaderName);
    if (!desc) {
        fprintf(stderr, "SpectralWireframe: failed to create UsdPreviewSurface schema\n");
        return nullptr;
    }

    _SetShaderProperties(*desc, rtx);
    shaderGroup.addShaderDesc(desc);
    RegisterParams();
    return desc;
}

void SpectralWireframeOp::updateShaderGraphOverrides(
    int32_t                outputType,
    const MaterialContext& rtx,
    usg::ShaderDescGroup&  shaderGroup)
{
    std::string shaderName = getShaderNodeName(rtx.shaderFamily);
    usg::ShaderDesc* desc = shaderGroup.getShaderNode(shaderName);
    if (desc) {
        _SetShaderProperties(*desc, rtx);
    }
    RegisterParams();
}
