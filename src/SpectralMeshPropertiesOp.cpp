// ---------------------------------------------------------------------------
// SpectralMeshPropertiesOp.cpp -- per-mesh render attribute overrides (GeomOp)
// ---------------------------------------------------------------------------

#include "SpectralMeshPropertiesOp.h"
#include <DDImage/Knobs.h>

using namespace DD::Image;

static const char* const kSubdivSchemeNames[] = {
    "auto (from mesh)", "catmullClark", "loop", "bilinear", "none", nullptr
};
static const char* const kNormalModeNames[] = {
    "auto (from mesh)", "smooth", "faceted", "vertex normals", nullptr
};
static const char* const kOrientationNames[] = {
    "right-handed", "left-handed", nullptr
};
static const char* const kPurposeNames[] = {
    "default", "render", "proxy", "guide", nullptr
};

static Op* build(Node* node) { return new SpectralMeshPropertiesOp(node); }
const GeomOp::Description SpectralMeshPropertiesOp::description("SpectralMeshProperties", build);

SpectralMeshPropertiesOp::SpectralMeshPropertiesOp(Node* node)
    : GeomOp(node, BuildEngine<Engine>())
{
}

// Erase our registry entry on Op destruction. See SpectralSurfaceOp.cpp
// for rationale; rename still leaks under the old name.
SpectralMeshPropertiesOp::~SpectralMeshPropertiesOp()
{
    auto& reg = GetRegistry();
    auto it = reg.find(node_name());
    if (it != reg.end())
        reg.erase(it);
}

const char* SpectralMeshPropertiesOp::node_help() const
{
    return
        "SpectralMeshProperties -- per-mesh render attribute overrides.\n\n"
        "Inline geometry modifier for SpectralRenderer.\n"
        "Connect between geometry and GeoScene:\n"
        "  GeoCard -> SpectralMeshProperties -> GeoScene\n\n"
        "SUBDIVISION:\n"
        "  Override render-time subdivision level per-mesh.\n"
        "  Does not change the viewport mesh.\n\n"
        "NORMALS:\n"
        "  Control shading normal computation.\n\n"
        "DISPLAY:\n"
        "  Override vertex colour for ID mattes or QC.\n\n"
        "USD ATTRIBUTES:\n"
        "  double-sided, orientation, purpose, visibility.";
}

// ---------------------------------------------------------------------------
void SpectralMeshPropertiesOp::knobs(Knob_Callback f)
{
    GeomOp::knobs(f);

    Text_knob(f,
        "<b><font size='+1'>SpectralMeshProperties</font></b><br>"
        "<font color='#999'>Per-mesh render overrides for SpectralRenderer.<br>"
        "Connect between geometry and GeoScene.</font>"
    );
    Divider(f);

    // === Subdivision ===
    BeginGroup(f, "mesh_subdiv_grp", "Subdivision");
    {
        Text_knob(f,
            "<font color='#666' size='-1'>"
            "Render-time subdivision. Does not change the viewport mesh --<br>"
            "only affects SpectralRenderer output."
            "</font>"
        );
        Divider(f);
        Int_knob(f, &_subdivLevel, "subdiv_level", "level");
        SetRange(f, 0, 6);
        KnobModifiesAttribValues(f);
        Tooltip(f, "Subdivision iterations at render time.\n"
                   "0 = use SpectralRender node's global setting\n"
                   "1 = light (4x faces)\n"
                   "2 = medium (16x faces) -- good default\n"
                   "3 = high (64x faces) -- hero assets\n"
                   "4+ = very high -- close-up detail\n\n"
                   "Each level quadruples the face count.");
        Enumeration_knob(f, &_subdivScheme, kSubdivSchemeNames, "subdiv_scheme", "scheme");
        KnobModifiesAttribValues(f);
        Tooltip(f, "Subdivision algorithm:\n\n"
                   "auto -- use whatever the mesh specifies\n"
                   "catmullClark -- quads, smooth corners (most common)\n"
                   "loop -- triangles, good for triangle meshes\n"
                   "bilinear -- flat subdivision (no smoothing)\n"
                   "none -- disable subdivision entirely");
    }
    EndGroup(f);

    // === Normals ===
    BeginGroup(f, "mesh_normals_grp", "Normals");
    {
        Enumeration_knob(f, &_normalMode, kNormalModeNames, "normal_mode", "mode");
        KnobModifiesAttribValues(f);
        Tooltip(f, "How normals are computed for shading:\n\n"
                   "auto -- use mesh normals if present, else smooth\n"
                   "smooth -- interpolated vertex normals (soft edges)\n"
                   "faceted -- flat per-face normals (hard edges)\n"
                   "vertex normals -- force use of mesh vertex normals");
        Bool_knob(f, &_flipNormals, "flip_normals", "flip normals");
        KnobModifiesAttribValues(f);
        Tooltip(f, "Reverse normal direction.\n"
                   "Fixes inside-out geometry from bad winding order.");
    }
    EndGroup(f);

    // === Display ===
    BeginGroup(f, "mesh_display_grp", "Display Colour");
    {
        Text_knob(f,
            "<font color='#666' size='-1'>"
            "Override vertex colour (primvars:displayColor).<br>"
            "Useful for ID mattes, QC vis, or flat shading."
            "</font>"
        );
        Divider(f);
        Bool_knob(f, &_useDisplayColor, "use_display_color", "override display color");
        KnobModifiesAttribValues(f);
        Color_knob(f, _displayColor, "display_color", "color");
        KnobModifiesAttribValues(f);
        Float_knob(f, &_displayOpacity, "display_opacity", "opacity");
        SetRange(f, 0.0f, 1.0f);
        KnobModifiesAttribValues(f);
    }
    EndGroup(f);

    // === USD Attributes ===
    BeginGroup(f, "mesh_usd_grp", "USD Attributes");
    {
        Bool_knob(f, &_doubleSided, "double_sided", "double-sided");
        KnobModifiesAttribValues(f);
        Tooltip(f, "Render both sides of the mesh.\n"
                   "Off = back faces culled (faster).");
        Enumeration_knob(f, &_orientation, kOrientationNames, "orientation", "orientation");
        ClearFlags(f, Knob::STARTLINE);
        KnobModifiesAttribValues(f);
        Divider(f);
        Enumeration_knob(f, &_purpose, kPurposeNames, "purpose", "purpose");
        KnobModifiesAttribValues(f);
        Bool_knob(f, &_visible, "spec_visible", "visible");
        ClearFlags(f, Knob::STARTLINE);
        KnobModifiesAttribValues(f);
        Bool_knob(f, &_castsShadows, "casts_shadows", "casts shadows");
        KnobModifiesAttribValues(f);
    }
    EndGroup(f);
}

int SpectralMeshPropertiesOp::knob_changed(Knob* k)
{
    RegisterParams();
    return GeomOp::knob_changed(k);
}

void SpectralMeshPropertiesOp::_validate(bool forReal)
{
    // _validate runs before every evaluation, regardless of whether the
    // user has touched any knobs. This is more reliable than relying on
    // knob_changed (which only fires on UI changes) or on the Engine's
    // firstOp() callback (which was not guaranteed to return the owning
    // SpectralMeshPropertiesOp*).
    GeomOp::_validate(forReal);
    RegisterParams();
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
void SpectralMeshPropertiesOp::Engine::processScenegraph(usg::GeomSceneContext& context)
{
    // Pass through input geometry unchanged. Parameter registration is
    // handled by the Op's _validate() so we don't need to reach back to
    // the parent op from the Engine (firstOp() was unreliable here).
    GeomOpEngine::processScenegraph(context);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
std::unordered_map<std::string, SpectralMeshPropertiesOp::MeshProps>&
SpectralMeshPropertiesOp::GetRegistry()
{
    static std::unordered_map<std::string, MeshProps> s_registry;
    return s_registry;
}

void SpectralMeshPropertiesOp::RegisterParams()
{
    // If the node is disabled in Nuke, remove any stale entry from the
    // registry so downstream SpectralRenderIop sees no overrides for this
    // node. Re-enabling will re-add via the next _validate / knob_changed.
    if (node_disabled()) {
        GetRegistry().erase(node_name());
        return;
    }

    MeshProps p;
    p.subdivLevel     = _subdivLevel;
    p.subdivScheme    = _subdivScheme;
    p.normalMode      = _normalMode;
    p.flipNormals     = _flipNormals;
    p.displayColor[0] = _displayColor[0];
    p.displayColor[1] = _displayColor[1];
    p.displayColor[2] = _displayColor[2];
    p.useDisplayColor = _useDisplayColor;
    p.displayOpacity  = _displayOpacity;
    p.doubleSided     = _doubleSided;
    p.orientation     = _orientation;
    p.purpose         = _purpose;
    p.visible         = _visible;
    p.castsShadows    = _castsShadows;
    GetRegistry()[node_name()] = p;
}
