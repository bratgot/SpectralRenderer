#pragma once

// ---------------------------------------------------------------------------
// SpectralLight
//
//   Light representation for the spectral renderer.
//   Stores properties extracted from UsdLux prims and provides
//   spectral emission at any wavelength.
//
//   Supported CIE illuminants:
//     D65 (daylight ~6500K)
//     A   (tungsten ~2856K)
//     Custom blackbody (any temperature)
//     sRGB colour (converted to spectral via Gaussian basis)
// ---------------------------------------------------------------------------

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>

#include <cmath>
#include <algorithm>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

struct SpectralLight
{
    enum class Type {
        Distant,    // directional (sun)
        Sphere,     // point/sphere
        Rect,       // area rectangle
        Dome,       // environment dome (sky)
    };

    enum class Illuminant {
        D65,        // CIE D65 daylight
        A,          // CIE A (tungsten)
        Blackbody,  // Planckian at colorTemperature
        RGB,        // sRGB colour → spectral
    };

    Type        type       = Type::Distant;
    Illuminant  illuminant = Illuminant::D65;

    // Transform
    GfVec3f     position   = GfVec3f(0.f);
    GfVec3f     direction  = GfVec3f(0.f, -1.f, 0.f);  // for distant lights

    // Properties
    GfVec3f     color      = GfVec3f(1.f);
    float       intensity  = 1.0f;
    float       exposure   = 0.0f;       // 2^exposure multiplier
    float       colorTemperature = 6500.f;
    bool        enableColorTemperature = false;

    // Area light size
    float       radius = 0.5f;          // sphere
    float       width  = 1.f, height = 1.f;  // rect

    std::string name;

    // ------------------------------------------------------------------
    // Effective intensity (intensity * 2^exposure)
    // ------------------------------------------------------------------
    float EffectiveIntensity() const
    {
        return intensity * std::pow(2.f, exposure);
    }

    // ------------------------------------------------------------------
    // Spectral emission at a given wavelength (nm)
    //   Returns spectral radiance in arbitrary units.
    // ------------------------------------------------------------------
    float SpectralEmission(float lambda) const
    {
        float spectrum = 1.f;

        if (enableColorTemperature || illuminant == Illuminant::Blackbody) {
            spectrum = _Blackbody(lambda, colorTemperature);
        } else {
            switch (illuminant) {
                case Illuminant::D65:
                    spectrum = _D65(lambda);
                    break;
                case Illuminant::A:
                    spectrum = _IlluminantA(lambda);
                    break;
                case Illuminant::RGB:
                    spectrum = _RGBtoSpectral(lambda, color);
                    break;
                default:
                    spectrum = 1.f;
                    break;
            }
        }

        return spectrum * EffectiveIntensity();
    }

    // ------------------------------------------------------------------
    // Direction toward the light from a surface point
    //   For distant lights: constant direction
    //   For point/sphere: direction from surface to light position
    // ------------------------------------------------------------------
    GfVec3f DirectionFrom(const GfVec3f& surfacePos) const
    {
        if (type == Type::Distant || type == Type::Dome) {
            // Negate stored direction — stored as light's forward, we want toward-light
            return GfVec3f(-direction[0], -direction[1], -direction[2]);
        }
        GfVec3f d = position - surfacePos;
        float len = d.GetLength();
        return (len > 1e-6f) ? d / len : GfVec3f(0.f, 1.f, 0.f);
    }

    // ------------------------------------------------------------------
    // Distance attenuation for point/sphere/rect lights
    //   Returns 1.0 for distant/dome lights (no falloff)
    // ------------------------------------------------------------------
    float Attenuation(const GfVec3f& surfacePos) const
    {
        if (type == Type::Distant || type == Type::Dome) return 1.f;
        GfVec3f d = position - surfacePos;
        float dist2 = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
        return 1.f / std::max(dist2, 0.001f);
    }

private:
    // ------------------------------------------------------------------
    // Planck's blackbody radiation (normalized to peak=1)
    // ------------------------------------------------------------------
    static float _Blackbody(float lambda_nm, float temperature)
    {
        if (temperature < 100.f) return 0.f;
        const double lambda_m = lambda_nm * 1e-9;
        const double h = 6.62607015e-34;
        const double c = 2.99792458e8;
        const double k = 1.380649e-23;

        double x = (h * c) / (lambda_m * k * temperature);
        if (x > 500.0) return 0.f;  // underflow guard
        double B = (2.0 * h * c * c) / (lambda_m * lambda_m * lambda_m * lambda_m * lambda_m)
                 / (std::exp(x) - 1.0);

        // Normalize to peak = 1 (Wien's displacement: peak at ~2898/T μm)
        double peakLambda = 2.898e-3 / temperature;  // meters
        double xPeak = (h * c) / (peakLambda * k * temperature);
        double Bpeak = (2.0 * h * c * c)
                     / (peakLambda * peakLambda * peakLambda * peakLambda * peakLambda)
                     / (std::exp(xPeak) - 1.0);

        return static_cast<float>(Bpeak > 0.0 ? B / Bpeak : 0.0);
    }

    // ------------------------------------------------------------------
    // CIE D65 daylight — simplified 5-term approximation
    // ------------------------------------------------------------------
    static float _D65(float lambda)
    {
        // Approximate D65 as blackbody at 6504K (close enough for rendering)
        return _Blackbody(lambda, 6504.f);
    }

    // ------------------------------------------------------------------
    // CIE Illuminant A (tungsten, 2856K)
    // ------------------------------------------------------------------
    static float _IlluminantA(float lambda)
    {
        return _Blackbody(lambda, 2856.f);
    }

    // ------------------------------------------------------------------
    // sRGB → spectral (same Gaussian basis as SpectralMaterial)
    // ------------------------------------------------------------------
    static float _RGBtoSpectral(float lambda, const GfVec3f& rgb)
    {
        auto gauss = [](float l, float center, float sigma) -> float {
            float t = (l - center) / sigma;
            return std::exp(-0.5f * t * t);
        };

        return rgb[0] * gauss(lambda, 600.f, 55.f)
             + rgb[1] * gauss(lambda, 540.f, 50.f)
             + rgb[2] * gauss(lambda, 450.f, 40.f);
    }
};

PXR_NAMESPACE_CLOSE_SCOPE
