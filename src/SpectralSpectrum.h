#pragma once

// ---------------------------------------------------------------------------
// SpectralSpectrum
//
//   Spectral rendering utilities:
//     - CIE 1931 2° observer colour matching functions (380–780nm, 5nm steps)
//     - Wavelength → XYZ tristimulus conversion
//     - XYZ → linear sRGB conversion
//     - Uniform and stratified wavelength sampling
//
//   The hero wavelength technique: instead of tracing 3 channels (RGB)
//   per ray, we trace 1 wavelength per ray and accumulate XYZ tristimulus
//   values across many samples.  This naturally handles dispersion,
//   fluorescence, and spectral lensing — effects that RGB rendering
//   cannot represent correctly.
//
//   Reference: "Hero Wavelength Spectral Sampling" (Wilkie et al., 2014)
// ---------------------------------------------------------------------------

#include <pxr/base/gf/vec3f.h>

#include <cmath>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralSpectrum
{
public:
    // Visible spectrum range (nm)
    static constexpr float kLambdaMin = 380.f;
    static constexpr float kLambdaMax = 780.f;
    static constexpr float kLambdaRange = kLambdaMax - kLambdaMin;

    // ------------------------------------------------------------------
    // CIE 1931 2° observer, 5nm steps from 380–780nm (81 entries)
    //   These are the standard colour matching functions that define
    //   how the human eye responds to each wavelength.
    // ------------------------------------------------------------------
    static constexpr int kCIECount = 81;

    static constexpr float kCIE_X[kCIECount] = {
        0.0014f, 0.0022f, 0.0042f, 0.0076f, 0.0143f,
        0.0232f, 0.0435f, 0.0776f, 0.1344f, 0.2148f,
        0.2839f, 0.3285f, 0.3483f, 0.3481f, 0.3362f,
        0.3187f, 0.2908f, 0.2511f, 0.1954f, 0.1421f,
        0.0956f, 0.0580f, 0.0320f, 0.0147f, 0.0049f,
        0.0024f, 0.0093f, 0.0291f, 0.0633f, 0.1096f,
        0.1655f, 0.2257f, 0.2904f, 0.3597f, 0.4334f,
        0.5121f, 0.5945f, 0.6784f, 0.7621f, 0.8425f,
        0.9163f, 0.9786f, 1.0263f, 1.0567f, 1.0622f,
        1.0456f, 1.0026f, 0.9384f, 0.8544f, 0.7514f,
        0.6424f, 0.5419f, 0.4479f, 0.3608f, 0.2835f,
        0.2187f, 0.1649f, 0.1212f, 0.0874f, 0.0636f,
        0.0468f, 0.0329f, 0.0227f, 0.0158f, 0.0114f,
        0.0081f, 0.0058f, 0.0041f, 0.0029f, 0.0020f,
        0.0014f, 0.0010f, 0.0007f, 0.0005f, 0.0003f,
        0.0002f, 0.0002f, 0.0001f, 0.0001f, 0.0001f,
        0.0000f
    };

    static constexpr float kCIE_Y[kCIECount] = {
        0.0000f, 0.0001f, 0.0001f, 0.0002f, 0.0004f,
        0.0006f, 0.0012f, 0.0022f, 0.0040f, 0.0073f,
        0.0116f, 0.0168f, 0.0230f, 0.0298f, 0.0380f,
        0.0480f, 0.0600f, 0.0739f, 0.0910f, 0.1126f,
        0.1390f, 0.1693f, 0.2080f, 0.2586f, 0.3230f,
        0.4073f, 0.5030f, 0.6082f, 0.7100f, 0.7932f,
        0.8620f, 0.9149f, 0.9540f, 0.9803f, 0.9950f,
        1.0000f, 0.9950f, 0.9786f, 0.9520f, 0.9154f,
        0.8700f, 0.8163f, 0.7570f, 0.6949f, 0.6310f,
        0.5668f, 0.5030f, 0.4412f, 0.3810f, 0.3210f,
        0.2650f, 0.2170f, 0.1750f, 0.1382f, 0.1070f,
        0.0816f, 0.0610f, 0.0446f, 0.0320f, 0.0232f,
        0.0170f, 0.0119f, 0.0082f, 0.0057f, 0.0041f,
        0.0029f, 0.0021f, 0.0015f, 0.0010f, 0.0007f,
        0.0005f, 0.0004f, 0.0003f, 0.0002f, 0.0001f,
        0.0001f, 0.0001f, 0.0000f, 0.0000f, 0.0000f,
        0.0000f
    };

    static constexpr float kCIE_Z[kCIECount] = {
        0.0065f, 0.0105f, 0.0201f, 0.0362f, 0.0679f,
        0.1102f, 0.2074f, 0.3713f, 0.6456f, 1.0391f,
        1.3856f, 1.6230f, 1.7471f, 1.7826f, 1.7721f,
        1.7441f, 1.6692f, 1.5281f, 1.2876f, 1.0419f,
        0.8130f, 0.6162f, 0.4652f, 0.3533f, 0.2720f,
        0.2123f, 0.1582f, 0.1117f, 0.0782f, 0.0573f,
        0.0422f, 0.0298f, 0.0203f, 0.0134f, 0.0088f,
        0.0058f, 0.0039f, 0.0027f, 0.0021f, 0.0018f,
        0.0017f, 0.0014f, 0.0011f, 0.0010f, 0.0008f,
        0.0006f, 0.0003f, 0.0002f, 0.0002f, 0.0001f,
        0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
        0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
        0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
        0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
        0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
        0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f,
        0.0000f
    };

    // ------------------------------------------------------------------
    // Evaluate CIE matching functions at arbitrary wavelength (nm)
    //   Uses linear interpolation between 5nm table entries.
    // ------------------------------------------------------------------
    static void CIE_XYZ(float lambda, float& x, float& y, float& z)
    {
        if (lambda < kLambdaMin || lambda > kLambdaMax) {
            x = y = z = 0.f;
            return;
        }
        float t = (lambda - kLambdaMin) / 5.f;
        int i = static_cast<int>(t);
        float frac = t - i;
        i = std::min(i, kCIECount - 2);

        x = kCIE_X[i] * (1.f - frac) + kCIE_X[i + 1] * frac;
        y = kCIE_Y[i] * (1.f - frac) + kCIE_Y[i + 1] * frac;
        z = kCIE_Z[i] * (1.f - frac) + kCIE_Z[i + 1] * frac;
    }

    // ------------------------------------------------------------------
    // XYZ → linear RGB conversion matrices
    // ------------------------------------------------------------------
    enum class ColorSpace { sRGB = 0, ACEScg = 1, ACES2065 = 2 };

    // XYZ → linear sRGB (D65 white point, Rec.709 primaries)
    static GfVec3f XYZtoLinearRGB(float X, float Y, float Z)
    {
        float r =  3.2406f * X - 1.5372f * Y - 0.4986f * Z;
        float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
        float b =  0.0557f * X - 0.2040f * Y + 1.0570f * Z;
        return GfVec3f(r, g, b);
    }

    // XYZ → ACEScg (AP1 primaries, D60 adapted)
    static GfVec3f XYZtoACEScg(float X, float Y, float Z)
    {
        float r =  1.6410f * X - 0.3249f * Y - 0.2364f * Z;
        float g = -0.6636f * X + 1.6153f * Y + 0.0168f * Z;
        float b =  0.0117f * X - 0.0083f * Y + 0.9884f * Z;
        return GfVec3f(r, g, b);
    }

    // XYZ → ACES 2065-1 (AP0 primaries, D60)
    static GfVec3f XYZtoACES2065(float X, float Y, float Z)
    {
        float r =  1.0498f * X + 0.0000f * Y - 0.0001f * Z;
        float g = -0.4959f * X + 1.3735f * Y + 0.0982f * Z;
        float b =  0.0000f * X + 0.0000f * Y + 0.9913f * Z;
        return GfVec3f(r, g, b);
    }

    // Convert using specified color space
    static GfVec3f XYZtoRGB(float X, float Y, float Z, ColorSpace cs)
    {
        switch (cs) {
            case ColorSpace::ACEScg:  return XYZtoACEScg(X, Y, Z);
            case ColorSpace::ACES2065: return XYZtoACES2065(X, Y, Z);
            default:                   return XYZtoLinearRGB(X, Y, Z);
        }
    }

    // ------------------------------------------------------------------
    // Convert a single spectral radiance sample at wavelength lambda
    // into an XYZ tristimulus contribution.
    //
    //   The Monte Carlo estimator for ∫ L(λ)·x̄(λ) dλ with uniform
    //   sampling PDF = 1/kLambdaRange is:
    //       (1/N) Σ L(λᵢ)·x̄(λᵢ)·kLambdaRange
    //
    //   We also divide by the CIE Y integral (≈106.86) so that an
    //   equal-energy illuminant of unit spectral radiance produces Y≈1.
    //   Without this, raw XYZ values are ~100x too bright.
    // ------------------------------------------------------------------
    static constexpr float kCIE_Y_Integral = 106.856895f;  // Σ CIE_Y[i]*5nm

    static GfVec3f RadianceToXYZ(float radiance, float lambda)
    {
        float cx, cy, cz;
        CIE_XYZ(lambda, cx, cy, cz);
        float scale = kLambdaRange / kCIE_Y_Integral;
        return GfVec3f(radiance * cx * scale,
                       radiance * cy * scale,
                       radiance * cz * scale);
    }

    // ------------------------------------------------------------------
    // Sample a wavelength uniformly in [380, 780] nm.
    //   u: uniform random number in [0, 1)
    // ------------------------------------------------------------------
    static float SampleWavelength(float u)
    {
        return kLambdaMin + u * kLambdaRange;
    }

    // ------------------------------------------------------------------
    // Simple spectral reflectance curves for basic materials.
    // These are placeholders — Phase 4 will use full Disney BSDF.
    // ------------------------------------------------------------------

    /// Flat white diffuse: reflects all wavelengths equally.
    static float ReflectanceWhite(float /*lambda*/) { return 0.8f; }

    /// Approximate red: high reflectance above 600nm.
    static float ReflectanceRed(float lambda) {
        return (lambda > 600.f) ? 0.85f : 0.05f;
    }

    /// Approximate green: peak around 530nm.
    static float ReflectanceGreen(float lambda) {
        float t = (lambda - 530.f) / 50.f;
        return std::max(0.05f, 0.8f * std::exp(-t * t));
    }

    /// Approximate blue: high below 480nm.
    static float ReflectanceBlue(float lambda) {
        return (lambda < 480.f) ? 0.8f : 0.05f;
    }

    /// Normal-mapped colour: uses the surface normal to create a
    /// wavelength-dependent reflectance that produces the classic
    /// normal-as-colour visualisation but through spectral rendering.
    /// R channel peaks at long wavelengths (red ~620nm),
    /// G channel peaks at medium wavelengths (green ~530nm),
    /// B channel peaks at short wavelengths (blue ~460nm).
    static float ReflectanceFromNormal(float lambda,
                                        const GfVec3f& normal)
    {
        // Remap normal from [-1,1] to [0,1]
        float nr = normal[0] * 0.5f + 0.5f;
        float ng = normal[1] * 0.5f + 0.5f;
        float nb = normal[2] * 0.5f + 0.5f;

        // Spectral basis: each channel has a Gaussian response
        auto gauss = [](float l, float center, float sigma) {
            float t = (l - center) / sigma;
            return std::exp(-0.5f * t * t);
        };

        float rBasis = gauss(lambda, 620.f, 40.f);  // red channel
        float gBasis = gauss(lambda, 530.f, 35.f);  // green channel
        float bBasis = gauss(lambda, 460.f, 30.f);  // blue channel

        return nr * rBasis + ng * gBasis + nb * bBasis;
    }
};

PXR_NAMESPACE_CLOSE_SCOPE
