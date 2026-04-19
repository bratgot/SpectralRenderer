// ---------------------------------------------------------------------------
// SpectralMeshPropertiesOp.cpp -- per-mesh render attribute overrides (GeomOp)
// ---------------------------------------------------------------------------

#include "SpectralMeshPropertiesOp.h"
#include <DDImage/Knobs.h>
#include <cstring>
#include <unordered_set>

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
        "SpectralMeshProperties -- per-mesh render attribute overrides.\n"
        "\n"
        "Inline geometry modifier for SpectralRenderer. Connect between a\n"
        "geometry source and the GeoScene collector:\n"
        "\n"
        "    GeoCard -> SpectralMeshProperties -> GeoScene\n"
        "\n"
        "Targets only the mesh(es) flowing through this node's input\n"
        "chain, not every mesh in the scene. Stacking multiple\n"
        "SpectralMeshProperties nodes lets you apply different overrides\n"
        "to different meshes in the same scene.\n"
        "\n"
        "== SUBDIVISION ==\n"
        "Render-time subdivision only -- the viewport mesh is unchanged.\n"
        "Level 0 means 'use the SpectralRender node's global setting'.\n"
        "Levels 1-4 are usable; OpenSubdiv clamps higher internally.\n"
        "Loop scheme requires all-triangle input.\n"
        "\n"
        "== NORMALS ==\n"
        "Mode controls how shading normals are computed. 'faceted' is\n"
        "useful for low-poly geometry you want to stay chunky; 'smooth'\n"
        "re-derives normals from vertex positions even if the mesh has\n"
        "authored flat ones. 'flip normals' fixes inside-out winding.\n"
        "\n"
        "== DISPLAY COLOUR ==\n"
        "Override the mesh's primvars:displayColor / displayOpacity.\n"
        "Typical use: flat-colour ID mattes for downstream compositing,\n"
        "or driving shaders that read displayColor as a baseColor.\n"
        "\n"
        "== USD ATTRIBUTES ==\n"
        "double-sided: render both faces vs back-face-cull.\n"
        "orientation: winding interpretation (right- vs left-handed).\n"
        "purpose: render / proxy / guide -- follows USD convention.\n"
        "visible: hide the mesh at BVH build time (renders empty).\n"
        "casts shadows: when off, this mesh doesn't block shadow rays\n"
        "  from other meshes -- appears normally but casts no shadow.\n"
        "receives shadows: when off, this mesh is shaded as unoccluded\n"
        "  -- renders fully lit regardless of what's above it. Useful\n"
        "  for CG reflections of set pieces that shouldn't darken under\n"
        "  animated foreground elements.\n"
        "\n"
        "== NOTES ==\n"
        "- Matching: the node walks its input(0) chain upstream and\n"
        "  records the names of Geo* source nodes (GeoCard, GeoCube,\n"
        "  GeoSphere, etc.). SpectralRender matches mesh prim paths\n"
        "  against this list by last-component.\n"
        "- Disable (D): disabling this node erases its entry from the\n"
        "  render registry; the mesh renders with default settings.\n"
        "- The knob is 'spec_visible' not 'visible' because 'visible'\n"
        "  collides with a built-in DD::Image::Op knob name.";
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
        SetRange(f, 0, 4);
        KnobModifiesAttribValues(f);
        Tooltip(f, "Subdivision iterations at render time.\n"
                   "0 = use SpectralRender node's global setting\n"
                   "1 = light (4x faces)\n"
                   "2 = medium (16x faces) -- good default\n"
                   "3 = high (64x faces) -- hero assets\n"
                   "4 = very high (256x faces) -- close-up detail\n\n"
                   "Each level quadruples the face count. OpenSubdiv\n"
                   "clamps internally at 4; higher values render the same.");
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
        Tooltip(f, "When on, the 'color' knob below replaces the mesh's\n"
                   "primvars:displayColor. When off, the mesh's authored\n"
                   "displayColor passes through unchanged.");
        Color_knob(f, _displayColor, "display_color", "color");
        KnobModifiesAttribValues(f);
        Tooltip(f, "Per-mesh displayColor override. Drives the baseColor\n"
                   "of UsdPreviewSurface materials that reference\n"
                   "displayColor, or shows up as a flat colour when used\n"
                   "for ID mattes in downstream comp.");
        Float_knob(f, &_displayOpacity, "display_opacity", "opacity");
        SetRange(f, 0.0f, 1.0f);
        KnobModifiesAttribValues(f);
        Tooltip(f, "Multiplies the mesh's final opacity. 1.0 = fully\n"
                   "opaque (default), 0.0 = fully transparent. Useful for\n"
                   "dialing holdout meshes or cross-fading LODs.");
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
        Tooltip(f, "Face winding interpretation:\n\n"
                   "right-handed: counter-clockwise faces point out (USD default)\n"
                   "left-handed:  clockwise faces point out\n\n"
                   "Flips which side is 'front' without recomputing normals.\n"
                   "Prefer 'flip normals' above if you want to invert shading.");
        Divider(f);
        Enumeration_knob(f, &_purpose, kPurposeNames, "purpose", "purpose");
        KnobModifiesAttribValues(f);
        Tooltip(f, "USD purpose attribute -- controls where the mesh\n"
                   "appears in the output:\n\n"
                   "default: visible in all contexts\n"
                   "render:  only in final render, not viewport\n"
                   "proxy:   only in viewport, not in final render\n"
                   "guide:   annotations / reference, neither by default");
        Bool_knob(f, &_visible, "spec_visible", "visible");
        ClearFlags(f, Knob::STARTLINE);
        KnobModifiesAttribValues(f);
        Tooltip(f, "Show or hide this mesh in the render. When off, the\n"
                   "mesh is skipped at BVH build time -- it contributes\n"
                   "nothing to the image, not even shadows or reflections.\n\n"
                   "Knob name is 'spec_visible' because 'visible' collides\n"
                   "with a built-in DD::Image::Op knob of the same name.");
        Bool_knob(f, &_castsShadows, "casts_shadows", "casts shadows");
    Tooltip(f, "When off, this mesh doesn't block shadow rays from other meshes. "
               "The mesh still renders normally in the beauty pass.");
    Bool_knob(f, &_receivesShadows, "receives_shadows", "receives shadows");
    Tooltip(f, "When off, this mesh is shaded as if unoccluded: no shadows "
               "land on it. The mesh still casts shadows onto others (unless "
               "casts shadows is also off). Useful for keeping CG reflections "
               "of set pieces clean.");
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

// Recursive helper: walk upstream from `op` collecting names of geometry-
// producing Nuke nodes. A "producer" is any Op whose class starts with
// "Geo" (GeoCard, GeoCube, GeoSphere, GeoSceneGraphReader, GeoRead, etc.),
// which is Nuke's convention for source-geometry ops. Those node names
// default to being the prim-path component in the USD stage.
//
// Stops recursion at:
//   - null Op
//   - another SpectralMeshProperties (overrides take precedence downstream)
//   - depth 8 (safety against pathological graphs)
//
// Callers use a set<string> to dedupe across branching (e.g. GeoScene with
// multiple inputs).
static void _CollectUpstreamGeoNames(DD::Image::Op* op,
                                     std::unordered_set<std::string>& out,
                                     int depth = 0)
{
    if (!op || depth >= 8) return;

    const char* cls = op->Class();
    // Stop at another SpectralMeshProperties so its overrides dominate
    // the paths upstream of it, not this node's.
    if (cls && std::strcmp(cls, "SpectralMeshProperties") == 0 && depth > 0) return;

    // If the class name starts with "Geo", record it. This covers Nuke's
    // source-geom ops (GeoCard, GeoCube, GeoSphere, GeoSceneGraphReader,
    // GeoRead, etc.) as well as modifier ops (GeoScene, GeoTransform,
    // GeoBindMaterial) -- modifiers don't hurt because the renderer will
    // never see a prim path matching a modifier's node_name().
    if (cls && cls[0] == 'G' && cls[1] == 'e' && cls[2] == 'o') {
        out.insert(op->node_name());
    }

    const int nInputs = op->inputs();
    for (int i = 0; i < nInputs; ++i) {
        _CollectUpstreamGeoNames(op->input(i), out, depth + 1);
    }
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
    p.receivesShadows = _receivesShadows;

    // Walk input(0) upstream and collect names of geometry-producer Ops.
    // These become the target prim-path components for this registry entry.
    // SpectralRenderIop matches USD mesh prim paths against these names.
    {
        std::unordered_set<std::string> names;
        if (inputs() > 0) {
            _CollectUpstreamGeoNames(input(0), names, 0);
        }
        p.targetPrimPaths.reserve(names.size());
        for (const auto& n : names) p.targetPrimPaths.push_back(n);
    }

    GetRegistry()[node_name()] = p;
}
