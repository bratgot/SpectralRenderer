// ---------------------------------------------------------------------------
// SpectralSurfaceOp.cpp
// ---------------------------------------------------------------------------

#include "SpectralSurfaceOp.h"

#include <DDImage/Knobs.h>
#include <DDImage/Op.h>
#include <DDImage/Iop.h>
#include <DDImage/Row.h>
#include <atomic>

#include "usg/geom/ShaderDesc.h"
#include "usg/base/Value.h"

using namespace DD::Image;

// Global version counter — incremented on ANY SpectralSurface knob change
static std::atomic<int> s_spectralSurfaceVersion{0};
int GetSpectralSurfaceVersion() { return s_spectralSurfaceVersion.load(); }

static const char* const kSpectralPresetNames[] = {
    "custom",
    "\xe2\x94\x80\xe2\x94\x80 dielectrics \xe2\x94\x80\xe2\x94\x80",
    "glass", "diamond", "water", "ruby",
    "\xe2\x94\x80\xe2\x94\x80 metals \xe2\x94\x80\xe2\x94\x80",
    "copper", "gold", "silver", "aluminium",
    "\xe2\x94\x80\xe2\x94\x80 organic \xe2\x94\x80\xe2\x94\x80",
    "skin", "wood", "white paper", "concrete", "rubber", "jade",
    "\xe2\x94\x80\xe2\x94\x80 spectral \xe2\x94\x80\xe2\x94\x80",
    "CD/DVD", "soap bubble", "highlighter",
    "\xe2\x94\x80\xe2\x94\x80 creative \xe2\x94\x80\xe2\x94\x80",
    "kryptonite", "bioluminescence", "plasma",
    "\xe2\x94\x80\xe2\x94\x80 measured metals (RGL) \xe2\x94\x80\xe2\x94\x80",
    "chrome steel", "brushed nickel", "brass", "tungsten", "anodized blue",
    "\xe2\x94\x80\xe2\x94\x80 measured fabrics (RGL) \xe2\x94\x80\xe2\x94\x80",
    "silk", "velvet", "satin", "denim", "leather",
    "\xe2\x94\x80\xe2\x94\x80 measured coatings (RGL) \xe2\x94\x80\xe2\x94\x80",
    "car paint red", "car paint black", "pearl white", "porcelain",
    nullptr
};

const char* const SpectralSurfaceOp::CLASS = "SpectralSurface";

static Op* build(Node* node) { return new SpectralSurfaceOp(node); }
const Op::Description SpectralSurfaceOp::description(CLASS, build);

// ---------------------------------------------------------------------------
SpectralSurfaceOp::SpectralSurfaceOp(Node* node)
    : ShaderOp(node)
{
}

// Remove our registry entry when the Op is destroyed. This fires when
// the node is deleted, the script is closed, or the Op is re-created
// (e.g. Nuke rebuilding the DAG after a structural edit). Without this
// the map grows for the life of the process and the last-known state
// of deleted nodes lingers as phantom overrides on any mesh that
// matches by prim-path fallback.
//
// Rename: Nuke does not destroy the Op on a simple rename, so the old
// entry is NOT erased here. RegisterParams will insert a second entry
// under the new name and the old one persists until deletion. Known
// limitation -- tracked in ROADMAP tech debt.
SpectralSurfaceOp::~SpectralSurfaceOp()
{
    auto& reg = GetRegistry();
    auto it = reg.find(node_name());
    if (it != reg.end())
        reg.erase(it);
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
        "DIFFRACTION:\n"
        "  grating spacing — line spacing in micrometers (0=off)\n"
        "  1.6=CD, 0.5=butterfly wing. Creates rainbow iridescence.\n\n"
        "FLUORESCENCE:\n"
        "  absorb wavelength — UV/blue absorption center (nm)\n"
        "  emit wavelength — visible re-emission center (nm)\n"
        "  Only possible in spectral renderers (wavelength shift).\n\n"
        "SUBSURFACE SCATTERING:\n"
        "  scatter color — what colour scatters inside\n"
        "  radius — mean free path in world units\n"
        "  Red scatters further in skin. Spectral MFP per wavelength.\n\n"
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

    // Master preset dropdown. 42 entries with 8 header dividers
    // (kSpectralPresetNames). Headers are handled as no-ops in
    // _ApplyPreset. Mirrors the SpectralVolumeMaterial approach.
    BeginClosedGroup(f, "preset_grp", "Preset");
    {
        Enumeration_knob(f, &_spectralPreset, kSpectralPresetNames,
                         "preset", "material");
        Tooltip(f,
            "Physically-accurate material presets.\n"
            "\n"
            "Dielectrics:  glass, diamond, water, ruby\n"
            "Metals:       copper, gold, silver, aluminium (spectral n,k)\n"
            "Organic:      skin, wood, paper, concrete, rubber, jade\n"
            "Spectral:     CD/DVD, soap bubble, highlighter\n"
            "Creative:     kryptonite, bioluminescence, plasma\n"
            "RGL metals:   chrome, nickel, brass, tungsten, anodized blue\n"
            "Fabrics:      silk, velvet, satin, denim, leather\n"
            "Coatings:     car paint, pearl white, porcelain\n"
            "\n"
            "Dielectrics set opacity ~0.002 to enable refraction. Set\n"
            "refraction bounces to 8+ in SpectralRender for clean glass.\n"
            "\n"
            "Metals in the first category use per-wavelength complex IOR\n"
            "tables (Palik's Handbook). RGL metals use tuned metallic=1\n"
            "+ IOR + roughness approximations -- cheaper, slightly less\n"
            "accurate.\n"
            "\n"
            "Presets reset all material properties. Tweak sliders after\n"
            "selecting to fine-tune."
        );
        Text_knob(f,
            "<font color='#888' size='-1'>"
            "Picking a preset resets all properties. Tweak sliders after."
            "</font>"
        );
    }
    EndGroup(f);

    BeginGroup(f, "surface_grp", "Surface");
    {
        Color_knob(f, _diffuseColor, "diffuse_color", "diffuse color");
        Tooltip(f, "Base surface colour.");
        Float_knob(f, &_metallic,  "metallic",  "metallic");   SetRange(f, 0.f, 1.f);
        Tooltip(f, "0 = dielectric, 1 = metal.");
        Float_knob(f, &_roughness, "roughness", "roughness");  SetRange(f, 0.f, 1.f);
        Tooltip(f, "0 = mirror, 1 = matte.");
        Float_knob(f, &_ior,       "ior",       "IOR");        SetRange(f, 1.0f, 3.0f);
        Tooltip(f, "Index of refraction. 1.33=water, 1.5=glass, 2.42=diamond.");
        Float_knob(f, &_opacity,   "opacity",   "opacity");    SetRange(f, 0.f, 1.f);
        Tooltip(f, "1=opaque, 0.002=glass. Low values enable refraction.");
        Color_knob(f, _emissiveColor, "emissive_color", "emissive color");
        Tooltip(f, "Self-illumination colour. Black = no emission.");
        Float_knob(f, &_clearcoat, "clearcoat", "clearcoat");  SetRange(f, 0.f, 1.f);
        Float_knob(f, &_clearcoatRoughness, "clearcoat_roughness", "clearcoat roughness");
        SetRange(f, 0.f, 1.f);
    }
    EndGroup(f);

    BeginClosedGroup(f, "spectral_grp", "Spectral");
    {
        Float_knob(f, &_abbeNumber, "abbe_number", "dispersion (Abbe)");
        SetRange(f, 0.f, 50.f);
        Tooltip(f, "Abbe number controls chromatic dispersion.\n"
                   "0 = none, 60 = crown glass, 30 = flint, 55 = diamond.\n"
                   "Uses Sellmeier equation for physically accurate dispersion.");
        Float_knob(f, &_thinFilmThickness, "thin_film", "thin-film (nm)");
        SetRange(f, 0.f, 1000.f);
        Tooltip(f, "Thin-film interference coating in nm.\n"
                   "100=blue, 200=gold, 300=magenta, 500=rainbow.");
    }
    EndGroup(f);

    BeginClosedGroup(f, "texture_grp", "Texture");
    {
        Float_knob(f, &_textureBlend, "texture_blend", "texture blend");
        SetRange(f, 0.f, 1.f);
        Tooltip(f, "0=base colour only, 1=full texture, 0.5=mix.\n"
                   "Connect an image to the tex pipe.");
    }
    EndGroup(f);

    BeginGroup(f, "map_grp", "Map (bump / displacement)");
    {
        static const char* const kMapModeNames[] = { "bump", "displacement", nullptr };
        Enumeration_knob(f, &_mapMode, kMapModeNames, "map_mode", "mode");
        Tooltip(f, "What the map pipe does:\n"
                   "bump = perturb normals only (fast, no silhouette)\n"
                   "displacement = move vertices (needs subdivision)");
        Float_knob(f, &_bumpStrength, "bump_strength", "bump strength");
        SetRange(f, 0.f, 5.f);
        Tooltip(f, "Normal perturbation intensity (bump mode).");
        Float_knob(f, &_displacementScale, "displacement_scale", "disp scale");
        SetRange(f, 0.f, 10.f);
        Tooltip(f, "Vertex offset in world units (displacement mode).");
        Float_knob(f, &_displacementMidpoint, "displacement_midpoint", "midpoint");
        SetRange(f, -1.f, 1.f);
        static const char* const kDispTypeNames[] = { "scalar", "vector tangent (beta)", "vector object (beta)", nullptr };
        Enumeration_knob(f, &_dispType, kDispTypeNames, "disp_type", "disp type");
        Tooltip(f, "Displacement map type:\n"
                   "scalar = height map (red channel along normal)\n"
                   "vector tangent (beta) = RGB in tangent space\n"
                   "vector object (beta) = RGB as XYZ offset\n\n"
                   "Vector modes are in beta — they require RGB\n"
                   "displacement maps (e.g. from ZBrush, Mudbox).");
    }
    EndGroup(f);

    BeginClosedGroup(f, "volume_grp", "Volume absorption");
    {
        Color_knob(f, _absorptionColor, "absorption_color", "volume color");
        Tooltip(f, "What colour glass/liquid transmits. White=clear.");
        Float_knob(f, &_absorptionDensity, "absorption_density", "density");
        SetRange(f, 0.f, 10.f);
        Tooltip(f, "0=clear, 0.5=tinted, 2=deep, 5+=very dark.");
    }
    EndGroup(f);

    BeginClosedGroup(f, "diffraction_grp", "Diffraction");
    {
        Float_knob(f, &_gratingSpacing, "grating_spacing", "grating spacing");
        SetRange(f, 0.f, 5.f);
        Tooltip(f, "Line spacing in micrometers. 0=off.\n"
                   "1.6=CD/DVD, 0.5=butterfly wing.");
        Float_knob(f, &_gratingStrength, "grating_strength", "strength");
        SetRange(f, 0.f, 1.f);
    }
    EndGroup(f);

    BeginClosedGroup(f, "fluor_grp", "Fluorescence");
    {
        Float_knob(f, &_fluorAbsorb, "fluor_absorb", "absorb (nm)");
        SetRange(f, 300.f, 500.f);
        Tooltip(f, "UV/blue absorption center. 350=highlighter, 400=whitener.");
        Float_knob(f, &_fluorEmit, "fluor_emit", "emit (nm)");
        SetRange(f, 400.f, 700.f);
        Tooltip(f, "Visible re-emission center. 520=green, 580=yellow.");
        Float_knob(f, &_fluorStrength, "fluor_strength", "strength");
        SetRange(f, 0.f, 5.f);
    }
    EndGroup(f);

    BeginClosedGroup(f, "sss_grp", "Subsurface scattering");
    {
        Color_knob(f, _sssColor, "sss_color", "scatter color");
        Tooltip(f, "Black=off. Red=skin/wax. White=milk/marble. Green=jade.");
        Float_knob(f, &_sssRadius, "sss_radius", "radius");
        SetRange(f, 0.f, 10.f);
        Tooltip(f, "Mean free path in world units. 0.5=skin, 2.0=wax.");
    }
    EndGroup(f);

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
            "Normals recomputed from displaced geometry.<br>"
            "<br>"
            "<b>Diffraction gratings (GPU + CPU)</b><br>"
            "Grating equation: d(sin&theta;<sub>i</sub> + sin&theta;<sub>m</sub>) = m&lambda;<br>"
            "Periodic surface structure diffracts each wavelength at a different angle.<br>"
            "First 3 orders summed. CD=1.6&mu;m spacing, butterfly=0.5&mu;m.<br>"
            "<br>"
            "<b>Fluorescence (GPU + CPU)</b><br>"
            "Material absorbs UV/blue photons and re-emits at longer visible wavelengths.<br>"
            "Stokes shift: emission wavelength &gt; absorption wavelength (energy loss).<br>"
            "Gaussian absorption (30nm) and emission (40nm) bands.<br>"
            "Impossible in RGB renderers -- requires spectral wavelength tracking.<br>"
            "<br>"
            "<b>Subsurface scattering (GPU + CPU)</b><br>"
            "Light enters the surface and scatters inside before exiting nearby.<br>"
            "Spectral MFP: each wavelength scatters a different distance.<br>"
            "Red penetrates further in skin, blue is absorbed quickly.<br>"
            "Isotropic random walk with exponential step lengths."
        );
    }
    EndGroup(f);

    Divider(f);
    Text_knob(f,
        "<font color='#666' size='-1'>"
        "SpectralSurface v1.1 \xc2\xb7 Physically-based spectral material<br>"
        "Diffraction + Fluorescence on GPU + CPU \xc2\xb7 SSS on GPU + CPU<br>"
        "Created by Marten Blumen"
        "</font>"
    );
}

// ---------------------------------------------------------------------------
int SpectralSurfaceOp::knob_changed(Knob* k)
{
    // Single master preset dropdown. Apply only when the value has
    // actually changed from the last applied one; this prevents
    // re-entrant panel-widget callbacks from re-applying the same
    // preset during set_value push.
    //
    // CRITICAL: return 1 after preset apply, matching SpectralVolume-
    // Material. Without this Nuke's default ShaderOp handling rebuilds
    // the Op mid-callback and subsequent preset picks never reach the
    // current instance. Combined with member-first writes in the push
    // block (so set_value becomes a no-op that doesn't fire further
    // callbacks), this is what makes the preset panel behave reliably.
    if (k->is("preset") && _spectralPreset != _lastAppliedPreset) {
        _ApplyPreset(_spectralPreset);
        _lastAppliedPreset = _spectralPreset;
        RegisterParams();
        s_spectralSurfaceVersion.fetch_add(1);
        return 1;
    }

    // Always update registry + bump global version so SpectralRender detects changes
    RegisterParams();
    s_spectralSurfaceVersion.fetch_add(1);
    return ShaderOp::knob_changed(k);
}

// ---------------------------------------------------------------------------
// 2026-04-20 rewrite: mirrors SpectralVolumeMaterial's approach. Populate
// LOCAL variables from the preset table, then push to knobs via
// Knob::set_value. This is critical -- writing to members first then
// calling set_value is a no-op because Nuke's change detection sees the
// bound value already matches. Local-first-then-push makes the panel
// update reliably, and avoids the re-entrant callback storms that
// plagued the old 8-dropdown V2 design.
// ---------------------------------------------------------------------------
void SpectralSurfaceOp::_ApplyPreset(int preset)
{
    if (preset == 0) return;  // custom -- don't change anything
    // Header entries (dividers in the enum list) -- do nothing
    if (preset == 1 || preset == 6 || preset == 11 || preset == 18 || preset == 22
        || preset == 26 || preset == 32 || preset == 38) return;

    // Locals: defaults for a "fresh custom material" (no advanced features).
    float diffuse[3]  = { 0.8f, 0.8f, 0.8f };
    float metallic    = 0.f;
    float roughness   = 0.5f;
    float ior         = 1.5f;
    float opacity     = 1.f;
    float emissive[3] = { 0.f, 0.f, 0.f };
    float clearcoat   = 0.f;
    float clearRough  = 0.f;
    float abbe        = 0.f;
    float thinFilm    = 0.f;
    int   metalType   = 0;
    float absColor[3] = { 1.f, 1.f, 1.f };
    float absDensity  = 0.f;
    float gratingSp   = 0.f;
    float gratingStr  = 1.f;
    float flAbsorb    = 0.f;
    float flEmit      = 0.f;
    float flStrength  = 0.f;
    float sssCol[3]   = { 0.f, 0.f, 0.f };
    float sssRad      = 0.f;

    switch (preset) {
        // -- dielectrics --
        case 2: // glass
            diffuse[0]=diffuse[1]=diffuse[2]=0.95f;
            metallic=0.f; roughness=0.f; ior=1.52f;
            opacity=0.002f; abbe=58.f;
            break;
        case 3: // diamond
            diffuse[0]=diffuse[1]=diffuse[2]=0.97f;
            metallic=0.f; roughness=0.f; ior=2.42f;
            opacity=0.002f; abbe=55.f;
            break;
        case 4: // water
            diffuse[0]=0.95f; diffuse[1]=0.95f; diffuse[2]=0.98f;
            metallic=0.f; roughness=0.f; ior=1.333f;
            opacity=0.15f; abbe=55.f;
            absColor[0]=0.4f; absColor[1]=0.75f; absColor[2]=0.9f; absDensity=1.f;
            break;
        case 5: // ruby
            diffuse[0]=0.8f; diffuse[1]=0.05f; diffuse[2]=0.1f;
            metallic=0.f; roughness=0.05f; ior=1.77f; opacity=0.3f;
            absColor[0]=0.9f; absColor[1]=0.05f; absColor[2]=0.1f;
            absDensity=3.f; abbe=45.f;
            flAbsorb=410.f; flEmit=694.f; flStrength=1.5f;
            break;

        // -- metals (spectral n,k) --
        case 7: // copper
            diffuse[0]=0.95f; diffuse[1]=0.64f; diffuse[2]=0.54f;
            metallic=1.f; roughness=0.2f; ior=1.1f;
            metalType=2;
            break;
        case 8: // gold
            diffuse[0]=1.f; diffuse[1]=0.76f; diffuse[2]=0.33f;
            metallic=1.f; roughness=0.1f; ior=0.47f;
            metalType=1;
            break;
        case 9: // silver
            diffuse[0]=0.97f; diffuse[1]=0.96f; diffuse[2]=0.91f;
            metallic=1.f; roughness=0.05f; ior=0.18f;
            metalType=3;
            break;
        case 10: // aluminium
            diffuse[0]=0.91f; diffuse[1]=0.92f; diffuse[2]=0.92f;
            metallic=1.f; roughness=0.15f; ior=1.39f;
            metalType=4;
            break;

        // -- organic --
        case 12: // skin
            diffuse[0]=0.76f; diffuse[1]=0.57f; diffuse[2]=0.45f;
            roughness=0.5f; ior=1.4f;
            sssCol[0]=0.9f; sssCol[1]=0.4f; sssCol[2]=0.2f; sssRad=0.5f;
            break;
        case 13: // wood
            diffuse[0]=0.43f; diffuse[1]=0.30f; diffuse[2]=0.18f;
            roughness=0.7f;
            break;
        case 14: // white paper
            diffuse[0]=0.75f; diffuse[1]=0.73f; diffuse[2]=0.70f;
            roughness=0.9f;
            break;
        case 15: // concrete
            diffuse[0]=0.55f; diffuse[1]=0.53f; diffuse[2]=0.50f;
            roughness=0.95f;
            break;
        case 16: // rubber
            diffuse[0]=0.05f; diffuse[1]=0.05f; diffuse[2]=0.05f;
            roughness=0.85f;
            break;
        case 17: // jade
            diffuse[0]=0.15f; diffuse[1]=0.5f; diffuse[2]=0.2f;
            roughness=0.3f; ior=1.66f;
            sssCol[0]=0.2f; sssCol[1]=0.7f; sssCol[2]=0.25f; sssRad=0.3f;
            break;

        // -- spectral --
        case 19: // CD/DVD
            diffuse[0]=0.1f; diffuse[1]=0.1f; diffuse[2]=0.12f;
            metallic=0.8f; roughness=0.05f;
            gratingSp=0.46f; gratingStr=0.25f;
            break;
        case 20: // soap bubble
            diffuse[0]=0.95f; diffuse[1]=0.95f; diffuse[2]=0.98f;
            roughness=0.f; ior=1.33f; opacity=0.05f;
            thinFilm=350.f;
            break;
        case 21: // highlighter
            diffuse[0]=0.8f; diffuse[1]=1.f; diffuse[2]=0.1f;
            roughness=0.8f;
            flAbsorb=350.f; flEmit=520.f; flStrength=3.f;
            break;

        // -- creative --
        case 23: // kryptonite
            diffuse[0]=0.1f; diffuse[1]=0.9f; diffuse[2]=0.15f;
            roughness=0.3f; ior=1.8f; opacity=0.4f;
            flAbsorb=380.f; flEmit=540.f; flStrength=4.f;
            absColor[0]=0.2f; absColor[1]=0.95f; absColor[2]=0.3f;
            absDensity=1.5f;
            break;
        case 24: // bioluminescence
            diffuse[0]=0.05f; diffuse[1]=0.15f; diffuse[2]=0.2f;
            roughness=0.6f; ior=1.4f;
            emissive[0]=0.f; emissive[1]=0.5f; emissive[2]=0.8f;
            flAbsorb=400.f; flEmit=480.f; flStrength=2.f;
            break;
        case 25: // plasma
            diffuse[0]=0.02f; diffuse[1]=0.02f; diffuse[2]=0.05f;
            roughness=0.f; ior=1.f; opacity=0.1f;
            emissive[0]=0.6f; emissive[1]=0.2f; emissive[2]=1.f;
            flAbsorb=350.f; flEmit=450.f; flStrength=3.f;
            break;

        // -- measured metals (RGL/MERL approximations) --
        case 27: // chrome steel
            diffuse[0]=0.55f; diffuse[1]=0.56f; diffuse[2]=0.56f;
            metallic=1.f; roughness=0.03f; ior=2.75f;
            break;
        case 28: // brushed nickel
            diffuse[0]=0.66f; diffuse[1]=0.64f; diffuse[2]=0.58f;
            metallic=1.f; roughness=0.25f; ior=1.85f;
            break;
        case 29: // brass
            diffuse[0]=0.89f; diffuse[1]=0.74f; diffuse[2]=0.42f;
            metallic=1.f; roughness=0.15f; ior=1.18f;
            break;
        case 30: // tungsten
            diffuse[0]=0.52f; diffuse[1]=0.50f; diffuse[2]=0.47f;
            metallic=1.f; roughness=0.1f; ior=3.5f;
            break;
        case 31: // anodized blue
            diffuse[0]=0.12f; diffuse[1]=0.25f; diffuse[2]=0.55f;
            metallic=0.7f; roughness=0.2f; ior=1.8f;
            metalType=4; // aluminium base
            thinFilm=250.f;
            break;

        // -- measured fabrics (RGL approximations) --
        case 33: // silk
            diffuse[0]=0.85f; diffuse[1]=0.78f; diffuse[2]=0.72f;
            roughness=0.35f; ior=1.55f;
            clearcoat=0.3f; clearRough=0.1f;
            break;
        case 34: // velvet
            diffuse[0]=0.25f; diffuse[1]=0.05f; diffuse[2]=0.08f;
            roughness=0.95f;
            break;
        case 35: // satin
            diffuse[0]=0.82f; diffuse[1]=0.80f; diffuse[2]=0.75f;
            roughness=0.45f;
            clearcoat=0.5f; clearRough=0.15f;
            break;
        case 36: // denim
            diffuse[0]=0.10f; diffuse[1]=0.15f; diffuse[2]=0.30f;
            roughness=0.85f;
            break;
        case 37: // leather
            diffuse[0]=0.35f; diffuse[1]=0.22f; diffuse[2]=0.12f;
            roughness=0.6f;
            clearcoat=0.15f; clearRough=0.3f;
            break;

        // -- measured coatings (RGL approximations) --
        case 39: // car paint red
            diffuse[0]=0.65f; diffuse[1]=0.04f; diffuse[2]=0.04f;
            metallic=0.3f; roughness=0.15f;
            clearcoat=1.f; clearRough=0.02f;
            break;
        case 40: // car paint black
            diffuse[0]=0.02f; diffuse[1]=0.02f; diffuse[2]=0.02f;
            metallic=0.1f; roughness=0.05f;
            clearcoat=1.f; clearRough=0.01f;
            break;
        case 41: // pearl white
            diffuse[0]=0.92f; diffuse[1]=0.90f; diffuse[2]=0.88f;
            metallic=0.15f; roughness=0.2f; ior=1.6f;
            clearcoat=0.8f; clearRough=0.05f;
            thinFilm=180.f;
            break;
        case 42: // porcelain
            diffuse[0]=0.93f; diffuse[1]=0.92f; diffuse[2]=0.89f;
            roughness=0.15f; ior=1.52f;
            sssCol[0]=0.95f; sssCol[1]=0.9f; sssCol[2]=0.85f; sssRad=0.1f;
            break;

        default: break;
    }

    // Flush locals to knobs via set_value. This is the key to the
    // refresh: members still hold the PREVIOUS value until set_value
    // runs, so Nuke's change detection fires correctly and the panel
    // refreshes. Mirrors SpectralVolumeMaterial's reliable pattern.
    // CRITICAL: write MEMBERS FIRST, then set_value. Volume does this to
    // make set_value a no-op (bound value matches before the call) which
    // suppresses Nuke's deferred change-notification callbacks. Without
    // this, each pushed knob fires a fresh knob_changed, Nuke ends up
    // rebuilding the Op mid-cascade, and subsequent preset picks hit a
    // stale instance that never reaches our handler.
    _diffuseColor[0] = diffuse[0];
    _diffuseColor[1] = diffuse[1];
    _diffuseColor[2] = diffuse[2];
    _metallic             = metallic;
    _roughness            = roughness;
    _ior                  = ior;
    _opacity              = opacity;
    _emissiveColor[0]     = emissive[0];
    _emissiveColor[1]     = emissive[1];
    _emissiveColor[2]     = emissive[2];
    _clearcoat            = clearcoat;
    _clearcoatRoughness   = clearRough;
    _abbeNumber           = abbe;
    _thinFilmThickness    = thinFilm;
    _metalType            = metalType;
    _absorptionColor[0]   = absColor[0];
    _absorptionColor[1]   = absColor[1];
    _absorptionColor[2]   = absColor[2];
    _absorptionDensity    = absDensity;
    _gratingSpacing       = gratingSp;
    _gratingStrength      = gratingStr;
    _fluorAbsorb          = flAbsorb;
    _fluorEmit            = flEmit;
    _fluorStrength        = flStrength;
    _sssColor[0]          = sssCol[0];
    _sssColor[1]          = sssCol[1];
    _sssColor[2]          = sssCol[2];
    _sssRadius            = sssRad;

    auto setVec3 = [&](const char* name, const float v[3]) {
        if (Knob* k = knob(name)) {
            k->set_value(v[0], 0);
            k->set_value(v[1], 1);
            k->set_value(v[2], 2);
        }
    };
    auto setF = [&](const char* name, float v) {
        if (Knob* k = knob(name)) k->set_value(v);
    };
    setVec3("diffuse_color",     diffuse);
    setF("metallic",             metallic);
    setF("roughness",            roughness);
    setF("ior",                  ior);
    setF("opacity",              opacity);
    setVec3("emissive_color",    emissive);
    setF("clearcoat",            clearcoat);
    setF("clearcoat_roughness",  clearRough);
    setF("abbe_number",          abbe);
    setF("thin_film",            thinFilm);
    // _metalType written in member-first block above.
    setVec3("absorption_color",  absColor);
    setF("absorption_density",   absDensity);
    setF("grating_spacing",      gratingSp);
    setF("grating_strength",     gratingStr);
    setF("fluor_absorb",         flAbsorb);
    setF("fluor_emit",           flEmit);
    setF("fluor_strength",       flStrength);
    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
}

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
    if (node_disabled()) {
        GetRegistry().erase(node_name());
        return;
    }

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
        Iop* mapIop = dynamic_cast<Iop*>(input(1));
        if (mapIop) {
            p.mapIop = input(1);
            if (_mapMode == 1) p.dispIop = input(1);  // displacement mode
        }
    }
    p.mapMode = _mapMode;
    p.dispType = _dispType;
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
    if (idx == 0 || idx == 1) {
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
