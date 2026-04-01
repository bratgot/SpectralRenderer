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
        // Fresnel F0 — metallic uses base colour, dielectric uses ior
        // ----------------------------------------------------------
        float iorF0 = _IorToF0(mat.ior);
        float F0 = _Lerp(iorF0, baseRefl, mat.metallic);

        // Schlick Fresnel
        float F = _FresnelSchlick(F0, VdotH);

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
        float diffuse = (1.f - mat.metallic) * baseRefl * diffuseFactor / 3.14159f;

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
    // Sample a bounce direction (cosine-weighted hemisphere).
    //   Returns sampled direction L, and the BSDF value / pdf.
    //   For diffuse-dominant materials this is importance-sampled.
    //
    //   u1, u2: uniform random numbers in [0, 1)
    //   Returns throughput: BSDF(N,V,L) * NdotL / pdf
    // ------------------------------------------------------------------
    static GfVec3f SampleDirection(
        const SpectralMaterial& mat,
        const GfVec3f& N,
        const GfVec3f& V,
        float lambda,
        float u1, float u2,
        float& throughput)
    {
        // Cosine-weighted hemisphere sampling
        //   pdf = cos(theta) / pi
        //   For Lambertian diffuse: BSDF = albedo / pi
        //   throughput = (albedo/pi * cos) / (cos/pi) = albedo
        float r = std::sqrt(u1);
        float phi = 2.f * 3.14159f * u2;

        // Local tangent frame
        GfVec3f T, B;
        _MakeBasis(N, T, B);

        float x = r * std::cos(phi);
        float y = r * std::sin(phi);
        float z = std::sqrt(std::max(0.f, 1.f - u1));

        GfVec3f L = _Normalize(T * x + B * y + N * z);

        float NdotL = _Dot(N, L);
        if (NdotL <= 0.f) {
            throughput = 0.f;
            return L;
        }

        // Evaluate full BSDF at the sampled direction
        float bsdfVal = Evaluate(mat, N, V, L, lambda);

        // PDF = cos(theta) / pi
        float pdf = NdotL / 3.14159f;

        throughput = (pdf > 1e-7f) ? bsdfVal / pdf : 0.f;

        return L;
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
};

PXR_NAMESPACE_CLOSE_SCOPE
