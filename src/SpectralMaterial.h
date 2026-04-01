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
