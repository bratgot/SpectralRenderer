// ---------------------------------------------------------------------------
// SpectralSurfaceOp.cpp
// ---------------------------------------------------------------------------

#include "SpectralSurfaceOp.h"

#include <DDImage/Knobs.h>
#include <DDImage/Op.h>
#include <DDImage/Iop.h>
#include <DDImage/Row.h>

#include "usg/geom/ShaderDesc.h"
#include "usg/base/Value.h"

using namespace DD::Image;

static const char* const kSpectralPresetNames[] = {
    "custom", "glass", "diamond", "copper", "gold", "silver", "aluminium",
    "white paper", "concrete", "wood", "skin", "rubber", "water", nullptr
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
        "INPUTS:\n"
        "  tex  — base colour texture from any Nuke image node\n"
        "  disp — displacement map (red channel = height)\n\n"
        "SURFACE:\n"
        "  diffuse color — base colour / albedo\n"
        "  metallic — 0=dielectric, 1=metal\n"
        "  roughness — 0=mirror, 1=matte\n"
        "  IOR — index of refraction (1.5=glass, 2.42=diamond)\n"
        "  opacity — 1=opaque, 0.02=glass (enables refraction)\n\n"
        "SPECTRAL:\n"
        "  preset — quick setup for common materials\n"
        "  dispersion (Abbe) — rainbow splitting in glass\n"
        "    60=crown glass, 30=flint glass, 55=diamond\n"
        "  thin-film — iridescent coating thickness in nm\n\n"
        "TEXTURE:\n"
        "  texture blend — mix between base colour and tex input\n"
        "    0=base only, 1=full texture, 0.5=half-and-half\n\n"
        "VOLUME ABSORPTION (Beer-Lambert):\n"
        "  volume color — what colour the glass transmits\n"
        "    red=(1,0,0) for red glass, white=clear\n"
        "  density — absorption strength (0=clear, 2=deep colour)\n\n"
        "DISPLACEMENT:\n"
        "  scale — world-space amplitude\n"
        "  midpoint — 0=outward only, 0.5=centered\n\n"
        "PRESETS: glass, diamond, copper, gold, silver, aluminium,\n"
        "  white paper, concrete, wood, skin, rubber, water\n\n"
        "Connect via GeoBindMaterial to assign to geometry.\n"
        "Metal presets use measured spectral (n,k) data for\n"
        "physically correct wavelength-dependent reflectance.";
}

// ---------------------------------------------------------------------------
void SpectralSurfaceOp::knobs(Knob_Callback f)
{
    ShaderOp::knobs(f);

    Text_knob(f,
        "<b><font size='+1'>SpectralSurface</font></b><br>"
        "<font color='#999'>Physically-based spectral material for SpectralRenderer</font>"
    );
    Divider(f);

    Divider(f, "Surface");
    Color_knob(f, _diffuseColor, "diffuse_color", "diffuse color");
    Tooltip(f, "Base surface colour. For metals this tints\n"
               "the specular reflection. For dielectrics this\n"
               "is the diffuse albedo.");
    Float_knob(f, &_metallic,  "metallic",  "metallic");   SetRange(f, 0.f, 1.f);
    Tooltip(f, "0 = dielectric (plastic, glass, wood)\n"
               "1 = metal (gold, copper, aluminium)\n"
               "Metals have no diffuse, only specular.");
    Float_knob(f, &_roughness, "roughness", "roughness");  SetRange(f, 0.f, 1.f);
    Tooltip(f, "Microfacet roughness.\n"
               "0 = perfect mirror / smooth glass\n"
               "0.3 = polished surface\n"
               "0.7 = brushed metal / rough plastic\n"
               "1.0 = completely matte");
    Float_knob(f, &_ior,       "ior",       "IOR");        SetRange(f, 1.0f, 3.0f);
    Tooltip(f, "Index of refraction.\n"
               "1.0 = air, 1.33 = water, 1.5 = glass\n"
               "1.52 = crown glass, 2.42 = diamond\n"
               "Controls Fresnel reflections and refraction.");
    Float_knob(f, &_opacity,   "opacity",   "opacity");    SetRange(f, 0.f, 1.f);
    Tooltip(f, "Surface opacity.\n"
               "1.0 = fully opaque (default)\n"
               "0.02 = glass / transparent\n"
               "0.0 = invisible\n"
               "Low values enable refraction.");
    Color_knob(f, _emissiveColor, "emissive_color", "emissive color");
    Tooltip(f, "Self-illumination colour and intensity.\n"
               "Black = no emission. Bright values make\n"
               "the surface glow as a light source.");
    Float_knob(f, &_clearcoat, "clearcoat", "clearcoat");  SetRange(f, 0.f, 1.f);
    Tooltip(f, "Additional clear specular layer on top.\n"
               "0 = off, 1 = full clearcoat.\n"
               "Use for car paint, lacquered wood, wet surfaces.");
    Float_knob(f, &_clearcoatRoughness, "clearcoat_roughness", "clearcoat roughness");
    SetRange(f, 0.f, 1.f);
    Tooltip(f, "Roughness of the clearcoat layer.\n"
               "0 = glossy clear coat, 1 = matte clear coat.");

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

    Divider(f, "Texture");
    Float_knob(f, &_textureBlend, "texture_blend", "texture blend");
    SetRange(f, 0.f, 1.f);
    Tooltip(f, "How much the tex pipe texture replaces the base color.\n"
               "0 = base color only (texture ignored)\n"
               "1 = full texture (default)\n"
               "0.5 = 50/50 mix of base color and texture");

    Divider(f, "Bump");
    Float_knob(f, &_bumpStrength, "bump_strength", "bump strength");
    SetRange(f, 0.f, 5.f);
    Tooltip(f, "Strength of the bump map connected to the bump input.\n"
               "0 = no bump, 1 = normal strength, 2+ = exaggerated.\n"
               "The bump map perturbs the surface normal to add\n"
               "fine surface detail without changing geometry.\n"
               "Connect any greyscale image to the bump pipe.");

    Divider(f, "Volume absorption");
    Color_knob(f, _absorptionColor, "absorption_color", "volume color");
    Tooltip(f, "What colour the glass/liquid transmits.\n"
               "Red = red glass (absorbs green+blue)\n"
               "Amber = whisky/beer\n"
               "White = clear (no absorption)");
    Float_knob(f, &_absorptionDensity, "absorption_density", "density");
    SetRange(f, 0.f, 10.f);
    Tooltip(f, "How quickly light is absorbed inside the volume.\n"
               "0 = clear glass (no absorption)\n"
               "0.5 = lightly tinted\n"
               "2.0 = deeply coloured\n"
               "5+ = very dark / opaque looking");

    Divider(f, "Diffraction");
    Float_knob(f, &_gratingSpacing, "grating_spacing", "grating spacing");
    SetRange(f, 0.f, 5.f);
    Tooltip(f, "Diffraction grating line spacing in micrometers.\n"
               "0 = disabled (default)\n"
               "1.6 = CD/DVD\n"
               "0.5 = butterfly wing / peacock feather\n"
               "Creates rainbow iridescence from surface structure.");
    Float_knob(f, &_gratingStrength, "grating_strength", "strength");
    SetRange(f, 0.f, 1.f);
    Tooltip(f, "Blend between regular BSDF (0) and diffraction (1).");

    Divider(f, "Fluorescence");
    Float_knob(f, &_fluorAbsorb, "fluor_absorb", "absorb wavelength");
    SetRange(f, 300.f, 500.f);
    Tooltip(f, "UV/blue wavelength absorbed by the material (nm).\n"
               "0 = disabled\n"
               "350 = UV (highlighter pen)\n"
               "400 = violet (laundry whitener)\n"
               "450 = blue (some minerals)");
    Float_knob(f, &_fluorEmit, "fluor_emit", "emit wavelength");
    SetRange(f, 400.f, 700.f);
    Tooltip(f, "Visible wavelength re-emitted (nm).\n"
               "520 = green (highlighter)\n"
               "580 = yellow\n"
               "610 = orange/red (some corals)");
    Float_knob(f, &_fluorStrength, "fluor_strength", "strength");
    SetRange(f, 0.f, 5.f);
    Tooltip(f, "Fluorescence intensity.\n"
               "0 = off, 1 = natural, 2+ = exaggerated.");

    Divider(f, "Subsurface scattering");
    Color_knob(f, _sssColor, "sss_color", "scatter color");
    Tooltip(f, "Colour of light scattered beneath the surface.\n"
               "Black = disabled. Red = skin/wax.\n"
               "White = milk/marble. Green = jade.");
    Float_knob(f, &_sssRadius, "sss_radius", "radius");
    SetRange(f, 0.f, 10.f);
    Tooltip(f, "Mean free path in world units.\n"
               "0 = disabled\n"
               "0.1 = dense material (stone)\n"
               "0.5 = skin\n"
               "2.0 = wax/milk\n"
               "Larger = more translucent.");

    Divider(f, "Displacement");
    Float_knob(f, &_displacementScale, "displacement_scale", "scale");
    SetRange(f, 0.f, 10.f);
    Tooltip(f, "Displacement map amplitude in world units.\n"
               "0 = disabled (default)\n"
               "Connect a texture to the displacement input\n"
               "on the geometry node to provide the map.");

    Float_knob(f, &_displacementMidpoint, "displacement_midpoint", "midpoint");
    SetRange(f, -1.f, 1.f);
    Tooltip(f, "Displacement bias.\n"
               "0.0 = centered (default, grey = no offset)\n"
               "-1.0 = maximum inward bias\n"
               "1.0 = maximum outward bias");

    File_knob(f, &_displacementFile, "displacement_file", "map");
    Tooltip(f, "Displacement map image file (.exr, .hdr, .png, .jpg).\n"
               "Red channel is used as the displacement height.\n"
               "Leave empty to disable displacement.");

    Divider(f);
    BeginClosedGroup(f, "spectral_surface_info", "Spectral material - how it works");
    {
        Text_knob(f,
            "<b>Surface shading model</b><br>"
            "Uses the Disney Principled BSDF (Burley 2012) with GGX microfacet specular.<br>"
            "Metallic/roughness workflow: metallic=0 gives dielectric (plastic, glass),<br>"
            "metallic=1 gives conductor (metal). Roughness controls microfacet spread.<br>"
            "<br>"
            "<b>Spectral reflectance</b><br>"
            "RGB base colour is converted to a spectral curve using Gaussian basis functions:<br>"
            "R(&lambda;) = r&middot;G(&lambda;,630,30) + g&middot;G(&lambda;,532,30) + b&middot;G(&lambda;,460,25)<br>"
            "where G(&lambda;,&mu;,&sigma;) = e<sup>-(&lambda;-&mu;)&sup2;/2&sigma;&sup2;</sup>.<br>"
            "Each ray evaluates this at a single wavelength for true spectral transport.<br>"
            "<br>"
            "<b>Metal Fresnel</b><br>"
            "Metal presets use measured (n,k) optical constants from Palik's Handbook.<br>"
            "Exact conductor Fresnel: F = (R<sub>s</sub> + R<sub>p</sub>) / 2 with complex IOR.<br>"
            "9-point spectral data (380&ndash;780nm) for Au, Cu, Ag, Al, Fe, Ti.<br>"
            "Produces physically correct colour shift with viewing angle.<br>"
            "<br>"
            "<b>Dispersion (Cauchy model)</b><br>"
            "n(&lambda;) = n<sub>d</sub> + (n<sub>d</sub>&minus;1)/V<sub>d</sub> "
            "&middot; (587.6&minus;&lambda;) / (656.3&minus;486.1)<br>"
            "where V<sub>d</sub> = Abbe number. Higher V<sub>d</sub> = less dispersion.<br>"
            "Each wavelength refracts at a different angle &rarr; rainbow splitting.<br>"
            "<br>"
            "<b>Thin-film interference (Fabry-Perot)</b><br>"
            "Phase: &delta; = 4&pi; n<sub>film</sub> d cos&theta;<sub>t</sub> / &lambda;<br>"
            "Reflectance modulation: F<sub>film</sub> = F &middot; (1 + &frac12;cos&delta;)<br>"
            "Coating thickness in nm controls the interference colour pattern.<br>"
            "<br>"
            "<b>Beer-Lambert absorption</b><br>"
            "Light inside glass/liquid is absorbed: T(&lambda;) = e<sup>&minus;&sigma;(&lambda;)&middot;d</sup><br>"
            "where &sigma;(&lambda;) = &minus;ln(color<sub>&lambda;</sub>) &times; density.<br>"
            "Volume colour = what survives (red glass absorbs blue/green).<br>"
            "Thicker geometry = deeper colour saturation &mdash; physically correct.<br>"
            "<br>"
            "<b>Multiscatter GGX (Kulla-Conty 2017)</b><br>"
            "Compensates energy lost to multiple microfacet bounces at high roughness.<br>"
            "f<sub>ms</sub> = F<sub>avg</sub> &middot; (1&minus;E<sub>ss</sub>(V)) &middot; "
            "(1&minus;E<sub>ss</sub>(L)) / (&pi;(1&minus;E<sub>ss,avg</sub>))<br>"
            "Rough metals gain ~40&ndash;70% energy. Smooth surfaces unaffected.<br>"
            "<br>"
            "<b>Texture blending</b><br>"
            "The tex input pipe provides base colour from any Nuke image node.<br>"
            "Texture blend controls the mix: result = baseColor&times;(1&minus;b) + texture&times;b.<br>"
            "Works on both CPU and GPU render paths.<br>"
            "<br>"
            "<b>Displacement</b><br>"
            "Render-time displacement via OpenSubdiv Catmull-Clark subdivision.<br>"
            "Per-vertex: P&prime; = P + N &middot; (sample &minus; midpoint) &times; scale.<br>"
            "Normals recomputed from displaced geometry."
        );
    }
    EndGroup(f);

    Divider(f);
    Text_knob(f,
        "<font color='#666' size='-1'>"
        "SpectralSurface v1.0 \xc2\xb7 Physically-based spectral material<br>"
        "Created by Marten Blumen"
        "</font>"
    );
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
            _opacity = 0.002f; _abbeNumber = 58.f; _thinFilmThickness = 0.f;
            _metalType = 0;
            _absorptionColor[0]=1.f; _absorptionColor[1]=1.f; _absorptionColor[2]=1.f; _absorptionDensity=0.f;
            break;
        case 2: // diamond
            _diffuseColor[0] = _diffuseColor[1] = _diffuseColor[2] = 0.97f;
            _metallic = 0.0f; _roughness = 0.0f; _ior = 2.42f;
            _opacity = 0.002f; _abbeNumber = 55.f; _thinFilmThickness = 0.f;
            _metalType = 0;
            _absorptionColor[0]=1.f; _absorptionColor[1]=1.f; _absorptionColor[2]=1.f; _absorptionDensity=0.f;
            break;
        case 3: // copper
            _diffuseColor[0] = 0.95f; _diffuseColor[1] = 0.64f; _diffuseColor[2] = 0.54f;
            _metallic = 1.0f; _roughness = 0.2f; _ior = 1.1f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 2;  // copper spectral IOR
            break;
        case 4: // gold
            _diffuseColor[0] = 1.0f; _diffuseColor[1] = 0.76f; _diffuseColor[2] = 0.33f;
            _metallic = 1.0f; _roughness = 0.1f; _ior = 0.47f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 1;  // gold spectral IOR
            break;
        case 5: // silver
            _diffuseColor[0] = 0.97f; _diffuseColor[1] = 0.96f; _diffuseColor[2] = 0.91f;
            _metallic = 1.0f; _roughness = 0.05f; _ior = 0.18f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 3;  // silver spectral IOR
            break;
        case 6: // aluminium
            _diffuseColor[0] = 0.91f; _diffuseColor[1] = 0.92f; _diffuseColor[2] = 0.92f;
            _metallic = 1.0f; _roughness = 0.15f; _ior = 1.39f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 4;  // aluminium spectral IOR
            break;
        case 7: // white paper
            _diffuseColor[0] = 0.75f; _diffuseColor[1] = 0.73f; _diffuseColor[2] = 0.70f;
            _metallic = 0.0f; _roughness = 0.9f; _ior = 1.5f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 0;
            break;
        case 8: // concrete
            _diffuseColor[0] = 0.55f; _diffuseColor[1] = 0.53f; _diffuseColor[2] = 0.50f;
            _metallic = 0.0f; _roughness = 0.95f; _ior = 1.5f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 0;
            break;
        case 9: // wood
            _diffuseColor[0] = 0.43f; _diffuseColor[1] = 0.30f; _diffuseColor[2] = 0.18f;
            _metallic = 0.0f; _roughness = 0.7f; _ior = 1.5f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 0;
            break;
        case 10: // skin
            _diffuseColor[0] = 0.76f; _diffuseColor[1] = 0.57f; _diffuseColor[2] = 0.45f;
            _metallic = 0.0f; _roughness = 0.5f; _ior = 1.4f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 0;
            break;
        case 11: // rubber
            _diffuseColor[0] = 0.05f; _diffuseColor[1] = 0.05f; _diffuseColor[2] = 0.05f;
            _metallic = 0.0f; _roughness = 0.85f; _ior = 1.5f;
            _opacity = 1.0f; _abbeNumber = 0.f; _thinFilmThickness = 0.f;
            _metalType = 0;
            break;
        case 12: // water
            _diffuseColor[0] = 0.95f; _diffuseColor[1] = 0.95f; _diffuseColor[2] = 0.98f;
            _metallic = 0.0f; _roughness = 0.0f; _ior = 1.333f;
            _opacity = 0.15f; _abbeNumber = 55.f; _thinFilmThickness = 0.f;
            _metalType = 0;
            _absorptionColor[0]=0.4f; _absorptionColor[1]=0.75f; _absorptionColor[2]=0.9f; _absorptionDensity=1.0f;
            break;
        default: // custom — don't change anything
            break;
    }

    // Force knob UI update
    if (Op* op = getOp()) {
        if (Knob* k = op->knob("diffuse_color")) {
            k->set_value(_diffuseColor[0], 0);
            k->set_value(_diffuseColor[1], 1);
            k->set_value(_diffuseColor[2], 2);
        }
        if (Knob* k = op->knob("metallic"))    k->set_value(_metallic);
        if (Knob* k = op->knob("roughness"))   k->set_value(_roughness);
        if (Knob* k = op->knob("ior"))         k->set_value(_ior);
        if (Knob* k = op->knob("opacity"))     k->set_value(_opacity);
        if (Knob* k = op->knob("abbe_number")) k->set_value(_abbeNumber);
        if (Knob* k = op->knob("thin_film"))   k->set_value(_thinFilmThickness);
        if (Knob* k = op->knob("absorption_color")) {
            k->set_value(_absorptionColor[0], 0);
            k->set_value(_absorptionColor[1], 1);
            k->set_value(_absorptionColor[2], 2);
        }
        if (Knob* k = op->knob("absorption_density")) k->set_value(_absorptionDensity);
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

std::unordered_map<std::string, SpectralSurfaceOp::SpectralParams>&
SpectralSurfaceOp::GetRegistry()
{
    static std::unordered_map<std::string, SpectralParams> s_registry;
    return s_registry;
}

void SpectralSurfaceOp::RegisterParams()
{
    SpectralParams p;
    p.abbeNumber        = _abbeNumber;
    p.thinFilmThickness = _thinFilmThickness;
    p.displacementScale = _displacementScale;
    p.displacementMidpoint = _displacementMidpoint;
    p.displacementFile = (_displacementFile && _displacementFile[0]) ? _displacementFile : "";
    p.metalType = _metalType;
    p.textureBlend = _textureBlend;
    p.absorptionColor[0] = _absorptionColor[0];
    p.absorptionColor[1] = _absorptionColor[1];
    p.absorptionColor[2] = _absorptionColor[2];
    p.absorptionDensity = _absorptionDensity;
    // Store Iop pointers if connected
    if (inputs() > 0 && input(0)) {
        Iop* texIop = dynamic_cast<Iop*>(input(0));
        if (texIop) p.texIop = input(0);
    }
    if (inputs() > 1 && input(1)) {
        Iop* bumpIop = dynamic_cast<Iop*>(input(1));
        if (bumpIop) p.bumpIop = input(1);
    }
    if (inputs() > 2 && input(2)) {
        Iop* dispIop = dynamic_cast<Iop*>(input(2));
        if (dispIop) p.dispIop = input(2);
    }
    p.bumpStrength = _bumpStrength;
    p.gratingSpacing = _gratingSpacing;
    p.gratingStrength = _gratingStrength;
    p.fluorAbsorb = _fluorAbsorb;
    p.fluorEmit = _fluorEmit;
    p.fluorStrength = _fluorStrength;
    p.sssColor[0] = _sssColor[0]; p.sssColor[1] = _sssColor[1]; p.sssColor[2] = _sssColor[2];
    p.sssRadius = _sssRadius;
    GetRegistry()[node_name()] = p;
}

bool SpectralSurfaceOp::test_input(int idx, Op* op) const
{
    if (idx >= 0 && idx <= 2) {
        if (!op) return true;
        return dynamic_cast<Iop*>(op) != nullptr;
    }
    return ShaderOp::test_input(idx, op);
}

// ---------------------------------------------------------------------------
usg::ShaderDesc* SpectralSurfaceOp::createShaderGraph(
    int32_t                outputType,
    const MaterialContext& rtx,
    usg::ShaderDescGroup&  shaderGroup)
{
    std::string shaderName = getShaderNodeName(rtx.shaderFamily);

    usg::ShaderDesc* existing = shaderGroup.getShaderNode(shaderName);
    if (existing) return existing;

    usg::ShaderDesc* desc = usg::ShaderDesc::createFromSchema("UsdPreviewSurface", shaderName);
    if (!desc) {
        fprintf(stderr, "SpectralSurface: failed to create UsdPreviewSurface schema\n");
        return nullptr;
    }

    _SetShaderProperties(*desc, rtx);
    shaderGroup.addShaderDesc(desc);

    // Register spectral params so the renderer can find them
    RegisterParams();

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
        RegisterParams();
    }
}
