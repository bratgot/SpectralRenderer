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
        Dome,       // environment dome (sky/HDRI)
        Spot,       // spot light with cone
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
    GfVec3f     direction  = GfVec3f(0.f, -1.f, 0.f);  // for distant/spot lights

    // Properties
    GfVec3f     color      = GfVec3f(1.f);
    float       intensity  = 1.0f;
    float       exposure   = 0.0f;       // 2^exposure multiplier
    float       colorTemperature = 6500.f;
    bool        enableColorTemperature = false;

    // Area light size
    float       radius = 0.5f;          // sphere
    float       width  = 1.f, height = 1.f;  // rect

    // Spot light cone
    float       coneAngle    = 90.f;    // full cone angle in degrees
    float       coneSoftness = 0.f;     // 0 = hard edge, 1 = fully soft
    float       _cosConeAngle = 0.f;    // precomputed cos(coneAngle/2)
    float       _cosPenumbra  = 0.f;    // precomputed cos((coneAngle/2)*(1-softness))

    // Environment map (dome light)
    int         envTexId = -1;          // texture ID for HDRI map
    int         envWidth = 0, envHeight = 0;
    const float* envPixels = nullptr;   // pointer to loaded HDRI data (RGB)

    std::string name;

    // ------------------------------------------------------------------
    // Effective intensity (intensity * 2^exposure)
    // ------------------------------------------------------------------
    float EffectiveIntensity() const
    {
        return intensity * std::pow(2.f, exposure);
    }

    // Precompute cone angles for spot lights (call after setting coneAngle/coneSoftness)
    void PrecomputeCone()
    {
        float halfAngle = coneAngle * 0.5f * 3.14159f / 180.f;
        _cosConeAngle = std::cos(halfAngle);
        float innerAngle = halfAngle * (1.f - coneSoftness);
        _cosPenumbra = std::cos(innerAngle);
    }

    // Spot light attenuation: 1.0 inside inner cone, smooth falloff to outer
    float SpotAttenuation(const GfVec3f& surfacePos) const
    {
        if (type != Type::Spot) return 1.f;
        GfVec3f toSurface = surfacePos - position;
        float len = toSurface.GetLength();
        if (len < 1e-6f) return 1.f;
        toSurface /= len;
        float cosTheta = toSurface[0]*direction[0] + toSurface[1]*direction[1] + toSurface[2]*direction[2];

        if (cosTheta < _cosConeAngle) return 0.f;  // outside cone
        if (cosTheta > _cosPenumbra) return 1.f;    // inside inner cone
        // Smooth falloff in penumbra
        float t = (cosTheta - _cosConeAngle) / (_cosPenumbra - _cosConeAngle + 1e-7f);
        return t * t * (3.f - 2.f * t);  // smoothstep
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
    // Environment map emission: sample HDRI at a direction
    //   dir: world-space direction (normalized)
    //   lambda: wavelength in nm
    //   Returns spectral radiance from the environment map
    // ------------------------------------------------------------------
    float EnvironmentEmission(const GfVec3f& dir, float lambda) const
    {
        if (type != Type::Dome || !envPixels || envWidth <= 0 || envHeight <= 0) {
            return SpectralEmission(lambda);
        }

        // Lat-long mapping: direction → (u, v)
        float theta = std::acos(std::max(-1.f, std::min(1.f, dir[1])));  // elevation
        float phi = std::atan2(dir[0], -dir[2]);                          // azimuth
        float u = (phi + 3.14159f) / (2.f * 3.14159f);
        float v = theta / 3.14159f;

        // Bilinear sample
        float fx = u * (envWidth - 1);
        float fy = v * (envHeight - 1);
        int x0 = std::max(0, std::min(int(fx), envWidth - 1));
        int y0 = std::max(0, std::min(int(fy), envHeight - 1));
        int x1 = std::min(x0 + 1, envWidth - 1);
        int y1 = std::min(y0 + 1, envHeight - 1);
        float dx = fx - x0, dy = fy - y0;

        auto px = [&](int x, int y) -> GfVec3f {
            int idx = (y * envWidth + x) * 3;
            return GfVec3f(envPixels[idx], envPixels[idx+1], envPixels[idx+2]);
        };

        GfVec3f c00 = px(x0,y0), c10 = px(x1,y0), c01 = px(x0,y1), c11 = px(x1,y1);
        GfVec3f top = c00 * (1-dx) + c10 * dx;
        GfVec3f bot = c01 * (1-dx) + c11 * dx;
        GfVec3f rgb = top * (1-dy) + bot * dy;

        // Convert sampled RGB to spectral
        float spectrum = _RGBtoSpectral(lambda, rgb);
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
            return GfVec3f(-direction[0], -direction[1], -direction[2]);
        }
        // Sphere, Rect, Spot — direction from surface toward light
        GfVec3f d = position - surfacePos;
        float len = d.GetLength();
        return (len > 1e-6f) ? d / len : GfVec3f(0.f, 1.f, 0.f);
    }

    // ------------------------------------------------------------------
    // Sample a random point on the light surface for soft shadows.
    //   u1, u2: uniform random in [0,1)
    //   Returns a direction from surfacePos toward the sampled point.
    // ------------------------------------------------------------------
    GfVec3f SampleDirection(const GfVec3f& surfacePos, float u1, float u2) const
    {
        if (type == Type::Distant) {
            return GfVec3f(-direction[0], -direction[1], -direction[2]);
        }

        if (type == Type::Dome) {
            // Cosine-weighted hemisphere for environment sampling
            GfVec3f N(0.f, 1.f, 0.f);  // up
            GfVec3f up = (std::abs(N[1]) < 0.999f) ? GfVec3f(0,1,0) : GfVec3f(1,0,0);
            GfVec3f T = GfCross(up, N);
            float tlen = T.GetLength();
            if (tlen > 1e-6f) T /= tlen;
            GfVec3f B = GfCross(N, T);
            float r = std::sqrt(u1);
            float phi = 6.28318f * u2;
            return GfVec3f(T * (r * std::cos(phi)) + B * (r * std::sin(phi)) + N * std::sqrt(std::max(0.f, 1.f - u1)));
        }

        GfVec3f samplePos = position;

        if ((type == Type::Sphere || type == Type::Spot) && radius > 0.f) {
            // Uniform point on sphere surface
            float theta = 2.f * 3.14159f * u1;
            float phi = std::acos(1.f - 2.f * u2);
            float sp = std::sin(phi);
            samplePos[0] += radius * sp * std::cos(theta);
            samplePos[1] += radius * sp * std::sin(theta);
            samplePos[2] += radius * std::cos(phi);
        } else if (type == Type::Rect) {
            // Random point on rectangle
            samplePos[0] += (u1 - 0.5f) * width;
            samplePos[1] += (u2 - 0.5f) * height;
        }

        GfVec3f d = samplePos - surfacePos;
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
        float atten = 1.f / std::max(dist2, 0.001f);
        if (type == Type::Spot) atten *= SpotAttenuation(surfacePos);
        return atten;
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

        return rgb[0] * gauss(lambda, 630.f, 30.f)
             + rgb[1] * gauss(lambda, 532.f, 30.f)
             + rgb[2] * gauss(lambda, 460.f, 25.f);
    }
};

PXR_NAMESPACE_CLOSE_SCOPE
