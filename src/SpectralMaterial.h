#pragma once

// ---------------------------------------------------------------------------
// SpectralMaterial
//
//   Material representation for the spectral renderer.
//   Stores properties extracted from UsdPreviewSurface and converts
//   RGB base colour to a spectral reflectance curve.
//
//   Phase 4 progression:
//     4a: Read UsdPreviewSurface → diffuse color, roughness, metallic
//     4b: Spectral Disney BSDF evaluation
//     4c: Spectral light emission
//     4d: Multi-bounce path tracing
// ---------------------------------------------------------------------------

#include <pxr/base/gf/vec3f.h>

#include <cmath>
#include <algorithm>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

struct SpectralMaterial
{
    // UsdPreviewSurface properties
    GfVec3f baseColor    = GfVec3f(0.8f, 0.8f, 0.8f);  // diffuseColor
    float   metallic     = 0.0f;
    float   roughness    = 0.5f;
    float   ior          = 1.5f;
    float   opacity      = 1.0f;
    GfVec3f emissiveColor = GfVec3f(0.0f);
    float   clearcoat    = 0.0f;
    float   clearcoatRoughness = 0.0f;

    // Texture references (indices into SpectralScene texture table, -1 = none)
    int     baseColorTexId = -1;   // diffuseColor texture
    int     roughnessTexId = -1;   // roughness texture
    int     metallicTexId  = -1;   // metallic texture
    int     normalMapTexId = -1;   // normal map
    int     bumpMapTexId   = -1;   // bump/height map from Iop pipe
    float   bumpStrength   = 1.0f; // bump map intensity multiplier
    int     displacementTexId = -1; // displacement map
    float   displacementScale = 0.f; // world units, 0 = disabled
    float   displacementMidpoint = 0.f;
    int     displacementMode = 0;  // 0=scalar (height along N), 1=vector tangent, 2=vector object

    // Spectral-specific properties
    float   abbeNumber       = 0.0f;   // dispersion (0=none, ~60=crown glass, ~30=flint)
    float   thinFilmThickness = 0.0f;  // thin-film interference thickness in nm (0=disabled)
    int     metalType        = 0;      // 0=none, 1=gold, 2=copper, 3=silver, 4=aluminium, 5=iron, 6=titanium
    float   textureBlend     = 1.0f;   // 0=base color only, 1=full texture
    GfVec3f absorptionColor  = GfVec3f(1.f, 1.f, 1.f);  // volume color (white=clear)
    float   absorptionDensity = 0.f;   // 0=clear, higher=more absorption

    // Diffraction grating
    float   gratingSpacing = 0.f;   // μm (0=off, 1.6=CD, 0.5=butterfly)
    float   gratingStrength = 1.f;  // blend factor

    // Fluorescence
    float   fluorAbsorb    = 0.f;   // UV absorption center nm (0=off, ~350=highlighter)
    float   fluorEmit      = 0.f;   // visible emission center nm (~520=green)
    float   fluorStrength  = 0.f;   // intensity (0=off, 1=strong)

    // Subsurface scattering
    GfVec3f sssColor       = GfVec3f(0.f);  // scattering colour (0=disabled)
    float   sssRadius      = 0.f;   // mean free path (world units, 0=disabled)

    // Identifier
    std::string name = "default";

    // ------------------------------------------------------------------
    // RGB → spectral reflectance
    //
    //   Converts an sRGB base colour to spectral reflectance at a given
    //   wavelength using Gaussian spectral basis functions.
    //   This is the "Smits-style" approach: red, green, blue channels
    //   each contribute a Gaussian centered at their dominant wavelength.
    //
    //   More physically accurate spectral upsampling (e.g. Jakob 2019)
    //   can replace this in a later phase.
    // ------------------------------------------------------------------
    float SpectralReflectance(float lambda) const
    {
        auto gauss = [](float l, float center, float sigma) -> float {
            float t = (l - center) / sigma;
            return std::exp(-0.5f * t * t);
        };

        // Tighter, better-separated Gaussians to avoid colour crosstalk.
        // Previous sigmas (55/50/40) caused red to leak into green → yellow.
        float rBasis = gauss(lambda, 630.f, 30.f);
        float gBasis = gauss(lambda, 532.f, 30.f);
        float bBasis = gauss(lambda, 460.f, 25.f);

        float reflectance = baseColor[0] * rBasis
                          + baseColor[1] * gBasis
                          + baseColor[2] * bBasis;

        return std::max(0.0f, std::min(1.0f, reflectance));
    }

    // ------------------------------------------------------------------
    // Spectral emission (for emissive surfaces)
    //   Returns spectral radiance at a given wavelength, proportional
    //   to the emissive colour.
    // ------------------------------------------------------------------
    float SpectralEmission(float lambda) const
    {
        if (emissiveColor[0] <= 0.f && emissiveColor[1] <= 0.f && emissiveColor[2] <= 0.f)
            return 0.f;

        auto gauss = [](float l, float center, float sigma) -> float {
            float t = (l - center) / sigma;
            return std::exp(-0.5f * t * t);
        };

        float rBasis = gauss(lambda, 630.f, 30.f);
        float gBasis = gauss(lambda, 532.f, 30.f);
        float bBasis = gauss(lambda, 460.f, 25.f);

        return emissiveColor[0] * rBasis
             + emissiveColor[1] * gBasis
             + emissiveColor[2] * bBasis;
    }

    // ------------------------------------------------------------------
    // Beer-Lambert spectral absorption
    //
    //   Returns transmittance for a ray traveling 'distance' through
    //   this material's volume. T(λ) = exp(-σ(λ) * distance).
    //
    //   absorptionColor = what colour survives (red glass = (1,0,0))
    //   absorptionDensity = how quickly light is absorbed
    //   σ(λ) = -log(color_at_λ) * density.  White = clear glass.
    // ------------------------------------------------------------------
    float SpectralTransmittance(float lambda, float distance) const
    {
        if (absorptionDensity <= 0.f || distance <= 0.f)
            return 1.f;

        auto gauss = [](float l, float center, float sigma) -> float {
            float t = (l - center) / sigma;
            return std::exp(-0.5f * t * t);
        };

        float rBasis = gauss(lambda, 630.f, 30.f);
        float gBasis = gauss(lambda, 532.f, 30.f);
        float bBasis = gauss(lambda, 460.f, 25.f);

        float colorAtLambda = absorptionColor[0] * rBasis
                            + absorptionColor[1] * gBasis
                            + absorptionColor[2] * bBasis;
        colorAtLambda = std::max(0.001f, std::min(1.f, colorAtLambda));

        float sigma = -std::log(colorAtLambda) * absorptionDensity;
        return std::exp(-sigma * distance);
    }

    /// Default white material
    static SpectralMaterial Default()
    {
        SpectralMaterial m;
        m.name = "default";
        return m;
    }
};

// Material index stored per-triangle — maps to a material table
using SpectralMaterialId = int;
static constexpr SpectralMaterialId kDefaultMaterialId = 0;

PXR_NAMESPACE_CLOSE_SCOPE
