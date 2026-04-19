// ---------------------------------------------------------------------------
// SpectralShadowCatcherOp.cpp — shadow catcher material
// ---------------------------------------------------------------------------

#include "SpectralShadowCatcherOp.h"
#include <DDImage/Knobs.h>
#include "usg/geom/ShaderDesc.h"
#include "usg/base/Value.h"

using namespace DD::Image;

const char* const SpectralShadowCatcherOp::CLASS = "SpectralShadowCatcher";

static Op* build(Node* node) { return new SpectralShadowCatcherOp(node); }
const Op::Description SpectralShadowCatcherOp::description(CLASS, build);

SpectralShadowCatcherOp::SpectralShadowCatcherOp(Node* node)
    : ShaderOp(node)
{
}

// Erase our registry entry on Op destruction. See SpectralSurfaceOp.cpp
// for rationale; rename still leaks under the old name.
SpectralShadowCatcherOp::~SpectralShadowCatcherOp()
{
    auto& reg = GetRegistry();
    auto it = reg.find(node_name());
    if (it != reg.end())
        reg.erase(it);
}

const char* SpectralShadowCatcherOp::node_help() const
{
    return
        "SpectralShadowCatcher — invisible shadow-receiving surface.\n\n"
        "Renders as a transparent surface that only captures shadows.\n"
        "Output: RGB = shadow colour, Alpha = shadow darkness.\n\n"
        "Connect via GeoBindMaterial to assign to geometry.";
}

void SpectralShadowCatcherOp::knobs(Knob_Callback f)
{
    ShaderOp::knobs(f);

    Text_knob(f,
        "<b><font size='+1'>SpectralShadowCatcher</font></b><br>"
        "<font color='#999'>Invisible shadow-receiving surface for compositing</font>"
    );
    Divider(f);

    Float_knob(f, &_shadowIntensity, "shadow_intensity", "shadow intensity");
    SetRange(f, 0.0f, 1.0f);
    Color_knob(f, _shadowColor, "shadow_color", "shadow colour");
    Bool_knob(f, &_selfShadow, "self_shadow", "self shadow");
}

int SpectralShadowCatcherOp::knob_changed(Knob* k)
{
    RegisterParams();
    return ShaderOp::knob_changed(k);
}

std::unordered_map<std::string, SpectralShadowCatcherOp::ShadowCatcherParams>&
SpectralShadowCatcherOp::GetRegistry()
{
    static std::unordered_map<std::string, ShadowCatcherParams> s_registry;
    return s_registry;
}

void SpectralShadowCatcherOp::RegisterParams()
{
    if (node_disabled()) {
        GetRegistry().erase(node_name());
        return;
    }

    ShadowCatcherParams p;
    p.shadowIntensity = _shadowIntensity;
    p.shadowColor[0]  = _shadowColor[0];
    p.shadowColor[1]  = _shadowColor[1];
    p.shadowColor[2]  = _shadowColor[2];
    p.selfShadow      = _selfShadow;
    GetRegistry()[node_name()] = p;
}

void SpectralShadowCatcherOp::_SetShaderProperties(usg::ShaderDesc& desc,
                                                     const MaterialContext& /*rtx*/)
{
    desc.overrideInput("opacity",   usg::Value(1.0f));
    desc.overrideInput("roughness", usg::Value(1.0f));
    desc.overrideInput("metallic",  usg::Value(0.0f));
}

usg::ShaderDesc* SpectralShadowCatcherOp::createShaderGraph(
    int32_t                outputType,
    const MaterialContext& rtx,
    usg::ShaderDescGroup&  shaderGroup)
{
    std::string shaderName = getShaderNodeName(rtx.shaderFamily);
    usg::ShaderDesc* existing = shaderGroup.getShaderNode(shaderName);
    if (existing) return existing;

    usg::ShaderDesc* desc = usg::ShaderDesc::createFromSchema("UsdPreviewSurface", shaderName);
    if (!desc) {
        fprintf(stderr, "SpectralShadowCatcher: failed to create UsdPreviewSurface schema\n");
        return nullptr;
    }

    _SetShaderProperties(*desc, rtx);
    shaderGroup.addShaderDesc(desc);
    RegisterParams();
    return desc;
}

void SpectralShadowCatcherOp::updateShaderGraphOverrides(
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
