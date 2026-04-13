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
#include <vector>

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
    GfVec3f     envAvgColor = GfVec3f(1.f); // average HDRI colour (computed at load)
    float       envRotation = 0.f;         // HDRI rotation in degrees
    float       envShadowSoftness = 0.5f;  // virtual light shadow softness (0=hard, 1=soft)

    // CDF importance sampling for HDRI
    std::vector<float> envConditionalCDF;  // per-row CDF (width entries per row)
    std::vector<float> envMarginalCDF;     // marginal CDF (height entries)
    std::vector<float> envRowIntegral;     // per-row luminance integral
    float       envCDFTotal = 0.f;         // total luminance sum
    bool        envHasCDF = false;

    // SH (Spherical Harmonics) L0+L1 bands — 4 coefficients × RGB
    // L0 = DC (average), L1 = 3 directional components
    GfVec3f     envSH[4] = { GfVec3f(0.f), GfVec3f(0.f), GfVec3f(0.f), GfVec3f(0.f) };
    bool        envHasSH = false;

    // Virtual lights — extracted from brightest HDRI regions
    struct VirtualLight {
        GfVec3f direction;  // normalized world direction
        GfVec3f color;      // RGB radiance
        float   solidAngle; // approximate solid angle of the bright region
    };
    std::vector<VirtualLight> envVirtualLights;

    // Evaluate SH lighting at a direction (returns RGB)
    GfVec3f EvalSH(const GfVec3f& dir) const {
        if (!envHasSH) return envAvgColor;
        // Y00 = 0.2821, Y1-1 = 0.4886*y, Y10 = 0.4886*z, Y11 = 0.4886*x
        return envSH[0] * 0.2821f
             + envSH[1] * (0.4886f * dir[1])
             + envSH[2] * (0.4886f * dir[2])
             + envSH[3] * (0.4886f * dir[0]);
    }

    // Compute average HDRI colour AND build importance sampling CDF
    void ComputeEnvAverage() {
        envHasCDF = false;
        if (!envPixels || envWidth <= 0 || envHeight <= 0) return;

        int W = envWidth, H = envHeight;
        double sumR=0, sumG=0, sumB=0;
        int total = W * H;

        // Build per-pixel luminance weighted by sin(theta) for solid angle
        envConditionalCDF.resize(total);
        envMarginalCDF.resize(H);
        envRowIntegral.resize(H);

        double grandTotal = 0.0;

        for (int y = 0; y < H; ++y) {
            // sin(theta) weighting: pixels near poles cover less solid angle
            float theta = 3.14159f * (float(y) + 0.5f) / float(H);
            float sinTheta = std::sin(theta);

            double rowSum = 0.0;
            for (int x = 0; x < W; ++x) {
                int idx = (y * W + x) * 3;
                float r = envPixels[idx], g = envPixels[idx+1], b = envPixels[idx+2];
                sumR += r; sumG += g; sumB += b;
                // Luminance weighted by solid angle
                float lum = (0.2126f*r + 0.7152f*g + 0.0722f*b) * sinTheta;
                rowSum += lum;
                envConditionalCDF[y * W + x] = float(rowSum);
            }
            // Normalize conditional CDF for this row
            if (rowSum > 1e-10) {
                float invRow = 1.f / float(rowSum);
                for (int x = 0; x < W; ++x) envConditionalCDF[y * W + x] *= invRow;
            } else {
                // Uniform fallback for black rows
                for (int x = 0; x < W; ++x) envConditionalCDF[y * W + x] = float(x+1) / float(W);
            }
            envRowIntegral[y] = float(rowSum);
            grandTotal += rowSum;
            envMarginalCDF[y] = float(grandTotal);
        }

        // Normalize marginal CDF
        if (grandTotal > 1e-10) {
            float invTotal = 1.f / float(grandTotal);
            for (int y = 0; y < H; ++y) envMarginalCDF[y] *= invTotal;
        } else {
            for (int y = 0; y < H; ++y) envMarginalCDF[y] = float(y+1) / float(H);
        }

        envCDFTotal = float(grandTotal);
        envHasCDF = (grandTotal > 1e-10);

        float inv = 1.f / std::max(1, total);
        envAvgColor = GfVec3f(float(sumR*inv), float(sumG*inv), float(sumB*inv));

        // ---- Compute SH L0+L1 bands ----
        // Project HDRI onto first 4 spherical harmonics (band 0 + band 1)
        envSH[0] = envSH[1] = envSH[2] = envSH[3] = GfVec3f(0.f);
        double shTotal = 0.0;
        for (int y = 0; y < H; ++y) {
            float v = (float(y) + 0.5f) / float(H);
            float theta = v * 3.14159f;
            float sinT = std::sin(theta), cosT = std::cos(theta);
            float dOmega = (2.f * 3.14159f / float(W)) * (3.14159f / float(H)) * sinT;

            for (int x = 0; x < W; ++x) {
                float u = (float(x) + 0.5f) / float(W);
                float phi = u * 2.f * 3.14159f - 3.14159f;
                // Direction for this pixel
                float dx = sinT * std::sin(phi);
                float dy = cosT;
                float dz = -sinT * std::cos(phi);

                int idx = (y * W + x) * 3;
                GfVec3f rgb(envPixels[idx], envPixels[idx+1], envPixels[idx+2]);

                // Y00 = 0.2821 (constant)
                // Y1-1 = 0.4886 * y, Y10 = 0.4886 * z, Y11 = 0.4886 * x
                envSH[0] += rgb * (0.2821f * dOmega);
                envSH[1] += rgb * (0.4886f * dy * dOmega);   // Y1-1 (up/down)
                envSH[2] += rgb * (0.4886f * dz * dOmega);   // Y10 (front/back)
                envSH[3] += rgb * (0.4886f * dx * dOmega);   // Y11 (left/right)
                shTotal += dOmega;
            }
        }
        envHasSH = (shTotal > 1e-6);

        // Rotate SH L1 coefficients by envRotation (Y-axis rotation)
        if (envHasSH && std::abs(envRotation) > 0.01f) {
            float rad = -envRotation * 3.14159f / 180.f;
            float cosR = std::cos(rad), sinR = std::sin(rad);
            // SH[3] = x component, SH[2] = z component — rotate around Y
            for (int ch = 0; ch < 3; ++ch) {
                float sx = envSH[3][ch], sz = envSH[2][ch];
                envSH[3][ch] = sx * cosR + sz * sinR;
                envSH[2][ch] = -sx * sinR + sz * cosR;
            }
        }

        // ---- Extract virtual lights from brightest HDRI regions ----
        // Downsample to grid, find cells with peak luminance well above average
        envVirtualLights.clear();
        const int gridW = 64, gridH = 32;
        float avgLum = 0.2126f*envAvgColor[0] + 0.7152f*envAvgColor[1] + 0.0722f*envAvgColor[2];

        struct GridCell { float peakLum; float r,g,b; float dx,dy,dz; float solidAngle; };
        std::vector<GridCell> grid(gridW * gridH, {0,0,0,0,0,0,0,0});

        for (int gy = 0; gy < gridH; ++gy) {
            float v = (float(gy) + 0.5f) / float(gridH);
            float theta = v * 3.14159f;
            float sinT = std::sin(theta), cosT = std::cos(theta);
            float dOmega = (2.f * 3.14159f / float(gridW)) * (3.14159f / float(gridH)) * sinT;
            int srcY0 = gy * H / gridH, srcY1 = std::min((gy+1) * H / gridH, H);

            for (int gx = 0; gx < gridW; ++gx) {
                float u = (float(gx) + 0.5f) / float(gridW);
                float phi = u * 2.f * 3.14159f - 3.14159f;
                int srcX0 = gx * W / gridW, srcX1 = std::min((gx+1) * W / gridW, W);

                // Find BRIGHTEST pixel in this cell (not average)
                float bestLum = 0.f;
                float br=0,bg=0,bb=0;
                for (int sy = srcY0; sy < srcY1; ++sy)
                    for (int sx = srcX0; sx < srcX1; ++sx) {
                        int si = (sy*W+sx)*3;
                        float pr=envPixels[si], pg=envPixels[si+1], pb=envPixels[si+2];
                        float pl = 0.2126f*pr + 0.7152f*pg + 0.0722f*pb;
                        if (pl > bestLum) { bestLum=pl; br=pr; bg=pg; bb=pb; }
                    }

                GridCell& c = grid[gy * gridW + gx];
                c.peakLum = bestLum;
                c.r=br; c.g=bg; c.b=bb;
                c.dx = sinT * std::sin(phi);
                c.dy = cosT;
                c.dz = -sinT * std::cos(phi);
                c.solidAngle = dOmega;
            }
        }

        // Pick brightest cells from UPPER hemisphere only (above horizon)
        // Require peak > 2× average luminance — ensures real HDR highlights
        float lumThreshold = std::max(0.1f, avgLum * 2.f);
        const int maxVL = 8;
        std::vector<bool> used(gridW * gridH, false);

        // Mark lower hemisphere as used (skip ground)
        for (int gy = gridH/2; gy < gridH; ++gy)
            for (int gx = 0; gx < gridW; ++gx)
                used[gy * gridW + gx] = true;
        for (int n = 0; n < maxVL; ++n) {
            int best = -1; float bestLum = 0.f;
            for (int i = 0; i < gridW * gridH; ++i) {
                if (!used[i] && grid[i].peakLum > bestLum) { bestLum = grid[i].peakLum; best = i; }
            }
            if (best < 0 || bestLum < lumThreshold) break;
            used[best] = true;

            // Suppress neighbors (don't pick adjacent cells for the same bright region)
            int gy = best / gridW, gx = best % gridW;
            for (int dy2=-2; dy2<=2; ++dy2) for (int dx2=-2; dx2<=2; ++dx2) {
                int ny=gy+dy2, nx=(gx+dx2+gridW)%gridW;  // wrap horizontally
                if (ny>=0 && ny<gridH)
                    used[ny*gridW+nx] = true;
            }

            GridCell& c = grid[best];

            // For LDR HDRIs: if this peak is barely above the first peak,
            // all cells have similar brightness — virtual lights aren't useful
            if (n > 0 && !envVirtualLights.empty()) {
                float firstLum = 0.2126f*envVirtualLights[0].color[0]
                               + 0.7152f*envVirtualLights[0].color[1]
                               + 0.0722f*envVirtualLights[0].color[2];
                if (c.peakLum < firstLum * 0.8f || c.peakLum / std::max(avgLum, 0.01f) < 2.f)
                    break;  // not enough contrast — stop adding
            }

            VirtualLight vl;
            vl.direction = GfVec3f(-c.dx, -c.dy, -c.dz);  // toward the light
            float dlen = vl.direction.GetLength();
            if (dlen > 1e-6f) vl.direction /= dlen;
            vl.color = GfVec3f(c.r, c.g, c.b);
            vl.solidAngle = c.solidAngle;
            envVirtualLights.push_back(vl);
        }

        // Rotate virtual light directions by envRotation
        if (!envVirtualLights.empty() && std::abs(envRotation) > 0.01f) {
            float rad = -envRotation * 3.14159f / 180.f;
            float cosR = std::cos(rad), sinR = std::sin(rad);
            for (auto& vl : envVirtualLights) {
                float x = vl.direction[0], z = vl.direction[2];
                vl.direction[0] = x * cosR + z * sinR;
                vl.direction[2] = -x * sinR + z * cosR;
            }
        }
    }

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

        // Lat-long mapping: direction → (u, v) with rotation
        float theta = std::acos(std::max(-1.f, std::min(1.f, dir[1])));  // elevation
        float phi = std::atan2(dir[0], -dir[2]);                          // azimuth
        // Apply HDRI rotation
        if (std::abs(envRotation) > 0.01f) {
            phi += envRotation * 3.14159f / 180.f;
            if (phi > 3.14159f) phi -= 2.f * 3.14159f;
            if (phi < -3.14159f) phi += 2.f * 3.14159f;
        }
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
    GfVec3f SampleDirection(const GfVec3f& surfacePos, float u1, float u2,
                            const GfVec3f& surfaceNormal = GfVec3f(0,1,0)) const
    {
        if (type == Type::Distant) {
            return GfVec3f(-direction[0], -direction[1], -direction[2]);
        }

        if (type == Type::Dome) {
            // HDRI importance sampling via 2D CDF (if available)
            if (envHasCDF && envWidth > 0 && envHeight > 0) {
                int W = envWidth, H = envHeight;

                // Sample row from marginal CDF
                auto lowerBound = [](const float* data, int n, float val) -> int {
                    int lo=0, hi=n;
                    while (lo < hi) { int mid=(lo+hi)/2; if (data[mid]<val) lo=mid+1; else hi=mid; }
                    return std::min(lo, n-1);
                };
                int y = lowerBound(envMarginalCDF.data(), H, u2);
                // Sample column from conditional CDF for this row
                int x = lowerBound(&envConditionalCDF[y * W], W, u1);

                // Convert pixel (x,y) → lat-long direction
                float u = (float(x) + 0.5f) / float(W);
                float v = (float(y) + 0.5f) / float(H);
                float phi = u * 2.f * 3.14159f - 3.14159f;  // [-pi, pi]
                // Reverse the HDRI rotation to get world-space direction
                if (std::abs(envRotation) > 0.01f)
                    phi -= envRotation * 3.14159f / 180.f;
                float theta = v * 3.14159f;                   // [0, pi]
                float sinT = std::sin(theta);
                GfVec3f dir(sinT * std::sin(phi), std::cos(theta), -sinT * std::cos(phi));
                float dlen = dir.GetLength();
                if (dlen > 1e-6f) dir /= dlen;
                return dir;
            }

            // Fallback: cosine-weighted hemisphere around surface normal
            GfVec3f N = surfaceNormal;
            float nlen = N.GetLength();
            if (nlen > 1e-6f) N /= nlen;
            GfVec3f up2 = (std::abs(N[1]) < 0.999f) ? GfVec3f(0,1,0) : GfVec3f(1,0,0);
            GfVec3f T = GfCross(up2, N);
            float tlen = T.GetLength();
            if (tlen > 1e-6f) T /= tlen;
            GfVec3f B = GfCross(N, T);
            float r = std::sqrt(u1);
            float phi = 6.28318f * u2;
            float x = r * std::cos(phi);
            float y = r * std::sin(phi);
            float z = std::sqrt(std::max(0.f, 1.f - u1));
            GfVec3f dir = T * x + B * y + N * z;
            float dlen = dir.GetLength();
            if (dlen > 1e-6f) dir /= dlen;
            return dir;
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
    // PDF of the light sampling strategy for MIS
    //   Returns the probability density of sampling direction L
    // ------------------------------------------------------------------
    float SamplePdf(const GfVec3f& surfacePos, const GfVec3f& L,
                    const GfVec3f& surfaceNormal) const
    {
        if (type == Type::Distant) {
            return 0.f;  // delta distribution — no MIS
        }
        if (type == Type::Dome) {
            if (envHasCDF && envPixels && envWidth > 0 && envHeight > 0) {
                // CDF-based PDF: p(direction) = luminance(u,v) * sinTheta / (totalLum * pixelSolidAngle)
                // Lat-long: direction → (u, v) → pixel
                float theta = std::acos(std::max(-1.f, std::min(1.f, L[1])));
                float phi = std::atan2(L[0], -L[2]);
                if (std::abs(envRotation) > 0.01f) {
                    phi += envRotation * 3.14159f / 180.f;
                    if (phi > 3.14159f) phi -= 2.f * 3.14159f;
                    if (phi < -3.14159f) phi += 2.f * 3.14159f;
                }
                float u = (phi + 3.14159f) / (2.f * 3.14159f);
                float v = theta / 3.14159f;
                int px = std::max(0, std::min(int(u * envWidth), envWidth - 1));
                int py = std::max(0, std::min(int(v * envHeight), envHeight - 1));
                int idx = (py * envWidth + px) * 3;
                float lum = 0.2126f * envPixels[idx] + 0.7152f * envPixels[idx+1]
                          + 0.0722f * envPixels[idx+2];
                float sinTheta = std::sin(theta);
                if (sinTheta < 1e-6f || envCDFTotal < 1e-10f) return 0.f;
                // PDF = (lum * sinTheta / totalLum) * (W * H) / (2 * pi * pi)
                // The 2*pi*pi is the integral of sinTheta over the sphere in (u,v) parameterization
                float pdf = (lum * sinTheta / envCDFTotal)
                          * float(envWidth * envHeight)
                          / (2.f * 3.14159f * 3.14159f);
                return std::max(0.f, pdf);
            }
            // Cosine-weighted hemisphere fallback
            float NdotL = L[0]*surfaceNormal[0] + L[1]*surfaceNormal[1] + L[2]*surfaceNormal[2];
            return std::max(0.f, NdotL) / 3.14159f;
        }
        if (type == Type::Sphere || type == Type::Spot) {
            if (radius <= 0.f) return 0.f;  // point light = delta
            // Uniform cone sampling: pdf = 1 / solid_angle
            GfVec3f toLight = position - surfacePos;
            float dist = toLight.GetLength();
            if (dist < 1e-6f) return 0.f;
            float sinThetaMax = std::min(1.f, radius / dist);
            float cosThetaMax = std::sqrt(std::max(0.f, 1.f - sinThetaMax * sinThetaMax));
            float solidAngle = 2.f * 3.14159f * (1.f - cosThetaMax);
            return (solidAngle > 1e-7f) ? 1.f / solidAngle : 0.f;
        }
        if (type == Type::Rect) {
            // Area sampling: pdf = dist² / (area * cos_theta_light)
            float area = width * height;
            if (area < 1e-7f) return 0.f;
            GfVec3f toLight = position - surfacePos;
            float dist2 = toLight[0]*toLight[0] + toLight[1]*toLight[1] + toLight[2]*toLight[2];
            float cosLight = std::abs(L[0]*direction[0] + L[1]*direction[1] + L[2]*direction[2]);
            return dist2 / (area * std::max(cosLight, 1e-6f));
        }
        return 0.f;
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
        // Sphere lights far from scene (>100 units) are "distant with soft shadow" — no falloff
        if (type == Type::Sphere && dist2 > 10000.f) return 1.f;
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
