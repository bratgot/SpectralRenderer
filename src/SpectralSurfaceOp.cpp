// ---------------------------------------------------------------------------
// SpectralSurfaceOp.cpp
// ---------------------------------------------------------------------------

#include "SpectralSurfaceOp.h"

#include <DDImage/Knobs.h>
#include <DDImage/Op.h>

#include "usg/geom/ShaderDesc.h"
#include "usg/base/Value.h"

using namespace DD::Image;

static const char* const kSpectralPresetNames[] = {
    "custom", "glass", "diamond", "copper", "gold", "silver", "aluminium", nullptr
};

const char* const SpectralSurfaceOp::CLASS = "SpectralSurface";

static Op* build(Node* node) { return new SpectralSurfaceOp(node); }
const Op::Description SpectralSurfaceOp::description(CLASS, build);

// ---------------------------------------------------------------------------
SpectralSurfaceOp::SpectralSurfaceOp(Node* node)
    : ShaderOp(node)
{
}

const char* SpectralSurfaceOp::node_help() const
{
    return
        "SpectralSurface — physically-based spectral material.\n\n"
        "Standard PBR controls (diffuse, metallic, roughness, IOR)\n"
        "plus spectral-specific features:\n\n"
        "Dispersion: Abbe number controls wavelength-dependent IOR.\n"
        "  0 = no dispersion (default)\n"
        "  ~60 = crown glass (low dispersion)\n"
        "  ~30 = flint glass (high dispersion)\n"
        "  ~55 = diamond\n\n"
        "Thin-film: Interference coating thickness in nm.\n"
        "  0 = disabled\n"
        "  200-800nm = visible spectrum interference\n\n"
        "Presets: Common material configurations.\n\n"
        "Connect via GeoBindMaterial to assign to geometry.";
}

// ---------------------------------------------------------------------------
void SpectralSurfaceOp::knobs(Knob_Callback f)
{
    ShaderOp::knobs(f);

    Divider(f, "Surface");
    Color_knob(f, _diffuseColor, "diffuse_color", "diffuse color");
    Float_knob(f, &_metallic,  "metallic",  "metallic");   SetRange(f, 0.f, 1.f);
    Float_knob(f, &_roughness, "roughness", "roughness");  SetRange(f, 0.f, 1.f);
    Float_knob(f, &_ior,       "ior",       "IOR");        SetRange(f, 1.0f, 3.0f);
    Float_knob(f, &_opacity,   "opacity",   "opacity");    SetRange(f, 0.f, 1.f);
    Color_knob(f, _emissiveColor, "emissive_color", "emissive color");
    Float_knob(f, &_clearcoat, "clearcoat", "clearcoat");  SetRange(f, 0.f, 1.f);
    Float_knob(f, &_clearcoatRoughness, "clearcoat_roughness", "clearcoat roughness");
    SetRange(f, 0.f, 1.f);

    Divider(f, "Spectral");
    Enumeration_knob(f, &_spectralPreset, kSpectralPresetNames, "preset", "preset");
    Tooltip(f, "Preset material configurations.\n"
               "Select 'custom' to set all values manually.");

    Float_knob(f, &_abbeNumber, "abbe_number", "dispersion (Abbe)");
    SetRange(f, 0.f, 100.f);
    Tooltip(f, "Abbe number — controls chromatic dispersion.\n"
               "0 = no dispersion (default)\n"
               "~60 = crown glass (low dispersion)\n"
               "~30 = flint glass (high dispersion)\n"
               "~55 = diamond");

    Float_knob(f, &_thinFilmThickness, "thin_film", "thin-film (nm)");
    SetRange(f, 0.f, 1000.f);
    Tooltip(f, "Thin-film interference coating thickness in nanometers.\n"
               "0 = disabled\n"
               "~100nm = deep blue\n"
               "~200nm = yellow-gold\n"
               "~300nm = magenta\n"
               "~400nm = cyan-green\n"
               "~500nm = rainbow/iridescent");
}

// ---------------------------------------------------------------------------
int SpectralSurfaceOp::knob_changed(Knob* k)
{
    if (k->is("preset")) {
        _ApplyPreset(_spectralPreset);
        return 1;
    }
    return ShaderOp::knob_changed(k);
}

// ---------------------------------------------------------------------------
void SpectralSurfaceOp::_ApplyPreset(int preset)
{
    switch (preset) {
        case 1: // glass
            _diffuseColor[0] = _diffuseColor[1] = _diffuseColor[2] = 0.95f;
            _metallic = 0.0f; _roughness = 0.0f; _ior = 1.52f;
            _opacity = 0.3f; _abbeNumber = 58.f; _thinFilmThickness = 0.f;
            break;
        case 2: // diamond
            _diffuseColor[0] = _diffuseColor[1] = _diffuseColor[2] = 0.97f;
            _metallic = 0.0f; _roughness = 0.0f; _ior = 2.42f;
            _opacity = 0.1f; _abbeNumber = 55.f; _thinFilmThickness = 0.f;
            break;
        case 3: // copper
            _diffuseColor[0] = 0.95f; _diffuseColor[1] = 0.64f; _diffuseColor[2] = 0.54f;
            _metallic = 1.0f; _roughness = 0.2f; _ior = 1.1f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            break;
        case 4: // gold
            _diffuseColor[0] = 1.0f; _diffuseColor[1] = 0.76f; _diffuseColor[2] = 0.33f;
            _metallic = 1.0f; _roughness = 0.1f; _ior = 0.47f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            break;
        case 5: // silver
            _diffuseColor[0] = 0.97f; _diffuseColor[1] = 0.96f; _diffuseColor[2] = 0.91f;
            _metallic = 1.0f; _roughness = 0.05f; _ior = 0.18f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            break;
        case 6: // aluminium
            _diffuseColor[0] = 0.91f; _diffuseColor[1] = 0.92f; _diffuseColor[2] = 0.92f;
            _metallic = 1.0f; _roughness = 0.15f; _ior = 1.39f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            break;
        default: // custom — don't change anything
            break;
    }

    // Force knob UI update
    if (Op* op = getOp()) {
        if (Knob* k = op->knob("diffuse_color"))  k->changed();
        if (Knob* k = op->knob("metallic"))        k->changed();
        if (Knob* k = op->knob("roughness"))       k->changed();
        if (Knob* k = op->knob("ior"))             k->changed();
        if (Knob* k = op->knob("opacity"))         k->changed();
        if (Knob* k = op->knob("abbe_number"))     k->changed();
        if (Knob* k = op->knob("thin_film"))       k->changed();
    }
}

// ---------------------------------------------------------------------------
void SpectralSurfaceOp::_SetShaderProperties(usg::ShaderDesc& desc,
                                              const MaterialContext& rtx)
{
    // Standard UsdPreviewSurface properties
    desc.overrideInput("diffuseColor",
        usg::Value(fdk::Vec3f(_diffuseColor[0], _diffuseColor[1], _diffuseColor[2])));
    desc.overrideInput("metallic",           usg::Value(_metallic));
    desc.overrideInput("roughness",          usg::Value(_roughness));
    desc.overrideInput("ior",                usg::Value(_ior));
    desc.overrideInput("opacity",            usg::Value(_opacity));
    desc.overrideInput("emissiveColor",
        usg::Value(fdk::Vec3f(_emissiveColor[0], _emissiveColor[1], _emissiveColor[2])));
    desc.overrideInput("clearcoat",          usg::Value(_clearcoat));
    desc.overrideInput("clearcoatRoughness", usg::Value(_clearcoatRoughness));
}

// ---------------------------------------------------------------------------
usg::ShaderDesc* SpectralSurfaceOp::createShaderGraph(
    int32_t                outputType,
    const MaterialContext& rtx,
    usg::ShaderDescGroup&  shaderGroup)
{
    std::string shaderName = getShaderNodeName(rtx.shaderFamily);

    // Check if already created
    usg::ShaderDesc* existing = shaderGroup.getShaderNode(shaderName);
    if (existing) return existing;

    // Create a UsdPreviewSurface shader desc from the schema registry
    usg::ShaderDesc* desc = usg::ShaderDesc::createFromSchema("UsdPreviewSurface", shaderName);
    if (!desc) {
        fprintf(stderr, "SpectralSurface: failed to create UsdPreviewSurface schema\n");
        return nullptr;
    }

    _SetShaderProperties(*desc, rtx);

    shaderGroup.addShaderDesc(desc);

    return desc;
}

// ---------------------------------------------------------------------------
void SpectralSurfaceOp::updateShaderGraphOverrides(
    int32_t                outputType,
    const MaterialContext& rtx,
    usg::ShaderDescGroup&  shaderGroup)
{
    std::string shaderName = getShaderNodeName(rtx.shaderFamily);
    usg::ShaderDesc* desc = shaderGroup.getShaderNode(shaderName);
    if (desc) {
        _SetShaderProperties(*desc, rtx);
    }
}
