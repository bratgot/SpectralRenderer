#pragma once

// ---------------------------------------------------------------------------
// SpectralBSDF
//
//   Simplified spectral Disney BSDF for direct lighting.
//   Evaluates diffuse + specular lobes at a single wavelength.
//
//   Inputs:
//     - SpectralMaterial (baseColor → spectral, roughness, metallic, ior)
//     - Surface normal N
//     - View direction V (toward camera)
//     - Light direction L (toward light)
//     - Wavelength lambda (nm)
//
//   Lobes:
//     Diffuse:  Disney diffuse (Burley 2012) with roughness-dependent
//               Fresnel at grazing angles.
//     Specular: GGX microfacet (Trowbridge-Reitz) with Smith-GGX geometry
//               and Schlick Fresnel. Metallic surfaces use baseColor as F0;
//               dielectrics use ior-derived F0.
//
//   Phase 4d will add importance sampling for bounce rays.
// ---------------------------------------------------------------------------

#include "SpectralMaterial.h"

#include <pxr/base/gf/vec3f.h>
#include <cmath>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

class SpectralBSDF
{
public:
    // ------------------------------------------------------------------
    // Evaluate the full BSDF * cos(theta) for direct lighting.
    //
    //   Returns: spectral reflectance * NdotL for this wavelength.
    //   Caller multiplies by incoming spectral radiance.
    //
    //   N, V, L must be normalized.
    // ------------------------------------------------------------------
    static float Evaluate(
        const SpectralMaterial& mat,
        const GfVec3f& N,      // surface normal
        const GfVec3f& V,      // view direction (toward camera)
        const GfVec3f& L,      // light direction (toward light)
        float lambda)           // wavelength in nm
    {
        float NdotL = _Dot(N, L);
        float NdotV = _Dot(N, V);

        if (NdotL <= 0.f || NdotV <= 0.f) return 0.f;

        // Half vector
        GfVec3f H = _Normalize(V + L);
        float NdotH = _Dot(N, H);
        float VdotH = _Dot(V, H);

        if (NdotH <= 0.f) return 0.f;

        float roughness = std::max(0.01f, mat.roughness);
        float alpha = roughness * roughness;

        // Spectral reflectance at this wavelength
        float baseRefl = mat.SpectralReflectance(lambda);

        // ----------------------------------------------------------
        // IOR with dispersion (wavelength-dependent)
        // Abbe number V_d: n(lambda) varies more for lower V_d
        // ----------------------------------------------------------
        float ior = mat.ior;
        if (mat.abbeNumber > 0.f) {
            ior = _DispersedIOR(mat.ior, mat.abbeNumber, lambda);
        }

        // ----------------------------------------------------------
        // Fresnel — conductor (spectral n,k) or dielectric (Schlick)
        // ----------------------------------------------------------
        float F;
        if (mat.metallic > 0.5f && mat.metalType > 0) {
            // Spectral conductor Fresnel with measured (n, k)
            float n_metal, k_metal;
            _GetMetalIOR(mat.metalType, lambda, n_metal, k_metal);
            F = _FresnelConductor(VdotH, n_metal, k_metal);
        } else {
            float iorF0 = _IorToF0(ior);
            float F0 = _Lerp(iorF0, baseRefl, mat.metallic);
            F = _FresnelSchlick(F0, VdotH);
        }

        // ----------------------------------------------------------
        // Thin-film interference modulation
        // ----------------------------------------------------------
        if (mat.thinFilmThickness > 0.f) {
            F = _ThinFilmFresnel(F, ior, mat.thinFilmThickness, lambda, VdotH);
        }

        // ----------------------------------------------------------
        // Specular: GGX microfacet
        // ----------------------------------------------------------
        float D = _GGX_D(alpha, NdotH);
        float G = _Smith_G(alpha, NdotV, NdotL);
        float specular = (D * G * F) / (4.f * NdotV * NdotL + 1e-7f);

        // ----------------------------------------------------------
        // Diffuse: Disney diffuse (Burley 2012)
        // ----------------------------------------------------------
        float FD90 = 0.5f + 2.f * VdotH * VdotH * roughness;
        float FDL = _SchlickWeight(NdotL);
        float FDV = _SchlickWeight(NdotV);
        float diffuseFactor = (1.f + (FD90 - 1.f) * FDL)
                            * (1.f + (FD90 - 1.f) * FDV);

        // Metals have no diffuse — only specular with coloured F0
        // Transparent materials (opacity < 1) reduce diffuse proportionally
        float diffuse = (1.f - mat.metallic) * mat.opacity * baseRefl * diffuseFactor / 3.14159f;

        return (diffuse + specular) * NdotL;
    }

    // ------------------------------------------------------------------
    // Evaluate against a hemisphere of sky lighting.
    // Uses 6-direction quadrature for speed (up, down, 4 sides).
    // Returns total outgoing spectral radiance at this wavelength.
    // ------------------------------------------------------------------
    static float EvaluateSkylighting(
        const SpectralMaterial& mat,
        const GfVec3f& N,
        const GfVec3f& V,
        float lambda,
        float (*skyFn)(const GfVec3f& dir, float lambda))
    {
        // 6-direction hemisphere quadrature
        static const GfVec3f dirs[] = {
            GfVec3f( 0.f,  1.f,  0.f),   // up
            GfVec3f( 0.f, -1.f,  0.f),   // down
            GfVec3f( 1.f,  0.f,  0.f),   // right
            GfVec3f(-1.f,  0.f,  0.f),   // left
            GfVec3f( 0.f,  0.f,  1.f),   // front
            GfVec3f( 0.f,  0.f, -1.f),   // back
        };
        static const float weights[] = {
            0.30f,  // up (strongest — sky)
            0.10f,  // down (ground bounce)
            0.15f, 0.15f, 0.15f, 0.15f  // sides (horizon)
        };

        float radiance = 0.f;
        for (int i = 0; i < 6; ++i) {
            float skyRad = skyFn(dirs[i], lambda);
            float bsdf = Evaluate(mat, N, V, dirs[i], lambda);
            radiance += bsdf * skyRad * weights[i];
        }

        // Emission
        radiance += mat.SpectralEmission(lambda);

        return radiance;
    }

    // ------------------------------------------------------------------
    // Sample a bounce direction — reflection or transmission.
    //   For opaque materials: GGX + cosine importance sampling
    //   For transparent materials (opacity < 1): Fresnel-weighted
    //     reflection vs refraction with wavelength-dependent IOR
    //
    //   u1, u2: uniform random numbers in [0, 1)
    //   throughput: BSDF weight / pdf for the sampled direction
    //   transmitted: true if the ray refracts through the surface
    // ------------------------------------------------------------------
    static GfVec3f SampleDirection(
        const SpectralMaterial& mat,
        const GfVec3f& N,
        const GfVec3f& V,
        float lambda,
        float u1, float u2,
        float& throughput,
        bool& transmitted,
        bool entering = true)
    {
        transmitted = false;
        float roughness = std::max(0.01f, mat.roughness);
        float alpha = roughness * roughness;

        // ----------------------------------------------------------
        // Transparent dielectric: Fresnel-weighted reflect vs refract
        // ----------------------------------------------------------
        if (mat.opacity < 0.99f && mat.metallic < 0.5f) {
            float ior = mat.ior;
            if (mat.abbeNumber > 0.f) ior = _DispersedIOR(mat.ior, mat.abbeNumber, lambda);

            // Compute eta and geometric normal for refraction
            // N is already flipped to face V. For refraction we need the
            // geometric orientation:
            //   entering: geoN = N (outward), eta = 1/ior
            //   exiting:  geoN = -N (outward), eta = ior/1
            float eta;
            GfVec3f geoN;
            if (entering) {
                eta = 1.f / ior;
                geoN = N;
            } else {
                eta = ior;
                geoN = -N;  // unflip to get outward-facing
            }

            float cosI = std::abs(V[0]*geoN[0] + V[1]*geoN[1] + V[2]*geoN[2]);
            float F = _FresnelDielectric(cosI, entering ? ior : 1.f/ior);

            if (u1 < F) {
                // Specular reflection
                GfVec3f L = _Reflect(V, N);
                throughput = 1.f;
                return L;
            } else {
                // Refraction — use geometric normal
                float sinT2 = eta * eta * (1.f - cosI * cosI);
                if (sinT2 > 1.f) {
                    // Total internal reflection
                    GfVec3f L = _Reflect(V, N);
                    throughput = 1.f;
                    return L;
                }
                float cosT = std::sqrt(1.f - sinT2);
                // Incident direction = -V, normal = geoN
                GfVec3f refracted = _Normalize(
                    GfVec3f(-V[0] * eta + geoN[0] * (eta * cosI - cosT),
                            -V[1] * eta + geoN[1] * (eta * cosI - cosT),
                            -V[2] * eta + geoN[2] * (eta * cosI - cosT)));
                transmitted = true;
                throughput = 1.f;  // clear glass, no absorption
                return refracted;
            }
        }

        // ----------------------------------------------------------
        // Opaque: GGX + cosine importance sampling (unchanged)
        // ----------------------------------------------------------
        float specProb = std::max(0.25f, 0.5f * (1.f - roughness) + 0.5f * mat.metallic);

        GfVec3f L;
        float pdf;

        GfVec3f T, B;
        _MakeBasis(N, T, B);

        if (u1 < specProb) {
            float xi1 = u1 / specProb;
            float xi2 = u2;

            float theta = std::atan(alpha * std::sqrt(xi1) / std::sqrt(std::max(1e-8f, 1.f - xi1)));
            float phi = 2.f * 3.14159f * xi2;

            float sinT = std::sin(theta);
            float cosT = std::cos(theta);
            GfVec3f H = _Normalize(T * (sinT * std::cos(phi))
                                  + B * (sinT * std::sin(phi))
                                  + N * cosT);

            float VdotH = _Dot(V, H);
            if (VdotH <= 0.f) { throughput = 0.f; return N; }
            L = _Normalize(H * (2.f * VdotH) - V);

            float NdotH = _Dot(N, H);
            float NdotL = _Dot(N, L);
            if (NdotL <= 0.f) { throughput = 0.f; return L; }

            float D = _GGX_D(alpha, NdotH);
            float pdfGGX = D * NdotH / (4.f * VdotH + 1e-7f);
            float pdfCos = NdotL / 3.14159f;
            pdf = specProb * pdfGGX + (1.f - specProb) * pdfCos;
        } else {
            float xi1 = (u1 - specProb) / (1.f - specProb);
            float xi2 = u2;

            float r = std::sqrt(xi1);
            float phi = 2.f * 3.14159f * xi2;
            float x = r * std::cos(phi);
            float y = r * std::sin(phi);
            float z = std::sqrt(std::max(0.f, 1.f - xi1));
            L = _Normalize(T * x + B * y + N * z);

            float NdotL = _Dot(N, L);
            if (NdotL <= 0.f) { throughput = 0.f; return L; }

            float pdfCos = NdotL / 3.14159f;
            GfVec3f H = _Normalize(V + L);
            float NdotH = _Dot(N, H);
            float VdotH = _Dot(V, H);
            float D = _GGX_D(alpha, NdotH);
            float pdfGGX = (VdotH > 1e-7f) ? D * NdotH / (4.f * VdotH) : 0.f;
            pdf = specProb * pdfGGX + (1.f - specProb) * pdfCos;
        }

        float NdotL = _Dot(N, L);
        if (NdotL <= 0.f || pdf < 1e-7f) { throughput = 0.f; return L; }

        float bsdfVal = Evaluate(mat, N, V, L, lambda);
        throughput = bsdfVal / pdf;

        return L;
    }

    // ------------------------------------------------------------------
    // Pdf — compute sampling PDF for a given direction
    // Used by MIS to weight light vs BSDF samples
    // ------------------------------------------------------------------
    static float Pdf(
        const SpectralMaterial& mat,
        const GfVec3f& N,
        const GfVec3f& V,
        const GfVec3f& L)
    {
        float NdotL = _Dot(N, L);
        float NdotV = _Dot(N, V);
        if (NdotL <= 0.f || NdotV <= 0.f) return 0.f;

        float roughness = std::max(0.01f, mat.roughness);
        float alpha = roughness * roughness;
        float specProb = std::max(0.25f, 0.5f * (1.f - roughness) + 0.5f * mat.metallic);

        GfVec3f H = _Normalize(V + L);
        float NdotH = _Dot(N, H);
        float VdotH = _Dot(V, H);

        float D = _GGX_D(alpha, NdotH);
        float pdfGGX = (VdotH > 1e-7f) ? D * NdotH / (4.f * VdotH) : 0.f;
        float pdfCos = NdotL / 3.14159f;

        return specProb * pdfGGX + (1.f - specProb) * pdfCos;
    }

    // ------------------------------------------------------------------
    // Power heuristic (beta=2) for MIS
    // ------------------------------------------------------------------
    static float MISWeight(float pdfA, float pdfB)
    {
        float a2 = pdfA * pdfA;
        float b2 = pdfB * pdfB;
        return a2 / (a2 + b2 + 1e-10f);
    }

private:
    static float _Dot(const GfVec3f& a, const GfVec3f& b)
    {
        return std::max(0.f, a[0]*b[0] + a[1]*b[1] + a[2]*b[2]);
    }

    static GfVec3f _Normalize(const GfVec3f& v)
    {
        float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        return (len > 1e-8f) ? v / len : GfVec3f(0.f, 1.f, 0.f);
    }

    static float _Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    // IOR → F0 (normal incidence Fresnel reflectance)
    static float _IorToF0(float ior)
    {
        float r = (ior - 1.f) / (ior + 1.f);
        return r * r;
    }

    // Reflect V around N
    static GfVec3f _Reflect(const GfVec3f& V, const GfVec3f& N)
    {
        float VdotN = V[0]*N[0] + V[1]*N[1] + V[2]*N[2];
        return _Normalize(N * (2.f * VdotN) - V);
    }

    // Exact Fresnel for dielectrics (not Schlick approximation)
    static float _FresnelDielectric(float cosI, float ior)
    {
        cosI = std::max(0.f, std::min(1.f, cosI));
        float sinT2 = (1.f - cosI * cosI) / (ior * ior);
        if (sinT2 > 1.f) return 1.f;  // total internal reflection
        float cosT = std::sqrt(std::max(0.f, 1.f - sinT2));
        float rS = (cosI - ior * cosT) / (cosI + ior * cosT);
        float rP = (ior * cosI - cosT) / (ior * cosI + cosT);
        return 0.5f * (rS * rS + rP * rP);
    }

    // Refract using pre-computed eta = n1/n2
    // V points toward camera, N points outward from surface (already flipped to face V)
    // Returns the refracted direction pointing away from the surface (into material or out)
    static bool _RefractEta(const GfVec3f& V, const GfVec3f& N, float eta, GfVec3f& refracted)
    {
        // Incident direction = -V (toward the surface)
        float cosI = V[0]*N[0] + V[1]*N[1] + V[2]*N[2];
        if (cosI < 0.f) cosI = 0.f;

        float sinT2 = eta * eta * (1.f - cosI * cosI);
        if (sinT2 > 1.f) return false;  // total internal reflection

        float cosT = std::sqrt(1.f - sinT2);
        // refracted = eta * (-V) + (eta * cosI - cosT) * N
        refracted = _Normalize(
            GfVec3f(-V[0] * eta + N[0] * (eta * cosI - cosT),
                    -V[1] * eta + N[1] * (eta * cosI - cosT),
                    -V[2] * eta + N[2] * (eta * cosI - cosT)));
        return true;
    }

    // ------------------------------------------------------------------
    // Dispersion: wavelength-dependent IOR from Abbe number
    //
    // The Abbe number V_d = (n_d - 1) / (n_F - n_C) where:
    //   n_d = IOR at 587.6 nm (sodium d-line)
    //   n_F = IOR at 486.1 nm (hydrogen F-line)
    //   n_C = IOR at 656.3 nm (hydrogen C-line)
    //
    // We use a linear dispersion model:
    //   n(lambda) ≈ n_d + (n_d - 1) / V_d * (587.6 - lambda) / (656.3 - 486.1)
    // ------------------------------------------------------------------
    static float _DispersedIOR(float ior_d, float abbeNumber, float lambda)
    {
        if (abbeNumber <= 0.f) return ior_d;
        float dispersion = (ior_d - 1.f) / abbeNumber;
        float dLambda = (587.6f - lambda) / (656.3f - 486.1f);
        return ior_d + dispersion * dLambda;
    }

    // ------------------------------------------------------------------
    // Thin-film interference
    //
    // Modulates Fresnel reflectance based on optical path difference
    // through a thin coating of thickness d (in nm).
    //
    // Phase difference: delta = 4*pi * n_film * d * cos(theta_t) / lambda
    // where n_film ≈ 1.5 (typical coating), theta_t from Snell's law.
    //
    // Reflectance modulation (simplified Fabry-Perot):
    //   F_film = F * (1 + A * cos(delta))
    // where A ≈ 0.5 for a visible but not extreme effect.
    // ------------------------------------------------------------------
    static float _ThinFilmFresnel(float F, float ior, float thickness_nm,
                                   float lambda, float cosTheta)
    {
        const float n_film = 1.5f;  // coating refractive index

        // Snell's law for angle inside the film
        float sinTheta = std::sqrt(std::max(0.f, 1.f - cosTheta * cosTheta));
        float sinThetaT = sinTheta / n_film;
        float cosThetaT = std::sqrt(std::max(0.f, 1.f - sinThetaT * sinThetaT));

        // Optical path difference → phase
        float delta = 4.f * 3.14159f * n_film * thickness_nm * cosThetaT / lambda;

        // Modulate Fresnel with interference
        float modulation = 0.5f * std::cos(delta);
        return std::max(0.f, std::min(1.f, F * (1.f + modulation)));
    }

    // Schlick Fresnel approximation
    static float _FresnelSchlick(float F0, float cosTheta)
    {
        float t = 1.f - cosTheta;
        float t2 = t * t;
        return F0 + (1.f - F0) * t2 * t2 * t;
    }

    // (1 - cos)^5 weight for Disney diffuse
    static float _SchlickWeight(float cosTheta)
    {
        float t = 1.f - cosTheta;
        float t2 = t * t;
        return t2 * t2 * t;
    }

    // GGX (Trowbridge-Reitz) normal distribution
    static float _GGX_D(float alpha, float NdotH)
    {
        float a2 = alpha * alpha;
        float denom = NdotH * NdotH * (a2 - 1.f) + 1.f;
        return a2 / (3.14159f * denom * denom + 1e-7f);
    }

    // Smith-GGX geometry term (height-correlated)
    static float _Smith_G(float alpha, float NdotV, float NdotL)
    {
        return _Smith_G1(alpha, NdotV) * _Smith_G1(alpha, NdotL);
    }

    static float _Smith_G1(float alpha, float NdotX)
    {
        float a2 = alpha * alpha;
        float denom = NdotX + std::sqrt(a2 + (1.f - a2) * NdotX * NdotX);
        return (2.f * NdotX) / (denom + 1e-7f);
    }

    // Build orthonormal tangent frame from N
    static void _MakeBasis(const GfVec3f& N, GfVec3f& T, GfVec3f& B)
    {
        GfVec3f up = (std::abs(N[1]) < 0.999f) ? GfVec3f(0.f, 1.f, 0.f) : GfVec3f(1.f, 0.f, 0.f);
        T = _Normalize(_Cross(up, N));
        B = _Cross(N, T);
    }

    static GfVec3f _Cross(const GfVec3f& a, const GfVec3f& b)
    {
        return GfVec3f(
            a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0]);
    }

    // ------------------------------------------------------------------
    // Fresnel conductor — exact unpolarized reflectance for metals
    //   n = refractive index, k = extinction coefficient
    //   Both vary with wavelength for real metals
    // ------------------------------------------------------------------
    static float _FresnelConductor(float cosTheta, float n, float k)
    {
        float cos2 = cosTheta * cosTheta;
        float n2 = n * n;
        float k2 = k * k;
        float n2k2 = n2 + k2;

        // s-polarization
        float Rs = (n2k2 - 2.f * n * cosTheta + cos2)
                 / (n2k2 + 2.f * n * cosTheta + cos2);

        // p-polarization
        float Rp = (n2k2 * cos2 - 2.f * n * cosTheta + 1.f)
                 / (n2k2 * cos2 + 2.f * n * cosTheta + 1.f);

        return std::max(0.f, std::min(1.f, 0.5f * (Rs + Rp)));
    }

    // ------------------------------------------------------------------
    // Spectral (n, k) data for metals — measured optical constants
    //   Sampled at 9 wavelengths from 380nm to 780nm (50nm steps)
    //   Interpolated linearly for intermediate wavelengths
    //   Sources: Palik, Handbook of Optical Constants of Solids
    // ------------------------------------------------------------------
    struct MetalIOR {
        float n[9];  // refractive index at 380,430,480,530,580,630,680,730,780nm
        float k[9];  // extinction coefficient
    };

    static void _GetMetalIOR(int metalType, float lambda, float& n, float& k)
    {
        // Measured data: {380, 430, 480, 530, 580, 630, 680, 730, 780} nm
        static const MetalIOR metals[] = {
            // 0: none (unused)
            {{1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0}},
            // 1: Gold (Au)
            {{1.66, 1.47, 1.29, 0.64, 0.28, 0.17, 0.16, 0.16, 0.17},
             {1.94, 1.95, 1.85, 2.20, 2.81, 3.24, 3.62, 4.00, 4.38}},
            // 2: Copper (Cu)
            {{1.27, 1.21, 1.14, 1.08, 0.47, 0.23, 0.21, 0.21, 0.24},
             {2.14, 2.37, 2.56, 2.60, 2.65, 2.98, 3.32, 3.67, 4.01}},
            // 3: Silver (Ag)
            {{0.05, 0.04, 0.04, 0.04, 0.05, 0.06, 0.07, 0.10, 0.12},
             {1.95, 2.35, 2.82, 3.29, 3.59, 3.92, 4.25, 4.58, 4.90}},
            // 4: Aluminium (Al)
            {{0.51, 0.57, 0.63, 0.72, 0.96, 1.24, 1.55, 1.80, 2.08},
             {4.40, 4.84, 5.30, 5.74, 6.18, 6.62, 7.04, 7.48, 7.90}},
            // 5: Iron (Fe)
            {{1.72, 1.86, 2.01, 2.18, 2.39, 2.56, 2.68, 2.78, 2.86},
             {2.88, 2.94, 3.01, 3.08, 3.16, 3.27, 3.39, 3.50, 3.60}},
            // 6: Titanium (Ti)
            {{1.60, 1.72, 1.86, 2.00, 2.14, 2.30, 2.48, 2.64, 2.76},
             {2.37, 2.48, 2.58, 2.68, 2.78, 2.90, 3.03, 3.14, 3.24}},
        };

        const int numMetals = sizeof(metals) / sizeof(metals[0]);
        if (metalType < 0 || metalType >= numMetals) { n = 1.f; k = 0.f; return; }

        const MetalIOR& m = metals[metalType];

        // Interpolate — wavelength to array index
        float t = (lambda - 380.f) / 50.f;  // 0..8
        t = std::max(0.f, std::min(7.99f, t));
        int i = static_cast<int>(t);
        float frac = t - float(i);

        n = m.n[i] * (1.f - frac) + m.n[i + 1] * frac;
        k = m.k[i] * (1.f - frac) + m.k[i + 1] * frac;
    }
};

PXR_NAMESPACE_CLOSE_SCOPE
