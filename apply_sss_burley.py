#!/usr/bin/env python3
"""
apply_sss_burley.py -- replace the wrap-diffuse SSS approximation on
GPU with a Christensen-Burley-style normalized diffusion profile.

This is v3 of the patch. v1 applied cleanly against an older snapshot
(pre-clearcoat) but v2 tried to patch evalBSDF ignoring the Phase 3
clearcoat lobe that now lives between SSS and the return statement.
v3 anchors the whole evalBSDF body as one edit so we don't get caught
by drift again.

Problems the patch fixes with the current wrap-diffuse SSS:
  - wrap capped at 0.5 (any sssRadius >= 1 stops scaling)
  - strength capped at 1 (sssRadius > 1 no longer intensifies)
  - no forward-scatter / thin-part / back-lit glow term
  - no per-wavelength scatter depth (d fixed by radius alone)
  - unreachable: old block sat AFTER the NdL<=0 early-exit return

New approach (Christensen & Burley 2015, simplified for single shade):

  (1) Front-lit wrap (NdL > 0):
      Burley equal-sum-of-two-exponentials profile with wavelength-
      dependent depth d. Smooth terminator rolloff.
  (2) Back-lit forward-scatter (NdL <= 0):
      exp(-|NdL|*2/d) transmission with d = sssRadius * albedoAtLambda.
      Red goes deeper than blue.

evalBSDF is restructured so:
  - NdV<=0 still short-circuits (can't shade a backside)
  - NdL<=0 only short-circuits when no SSS is involved (otherwise falls
    through to evaluate back-lit SSS)
  - diff + spec + clearcoat all run only when NdL>0 && NdH>0
  - SSS block is the Burley version, reachable for both cases
  - Final return multiplies the Lambert NdL only into (diff+spec); the
    SSS and clearcoat terms carry their own angular factors

File touched: src/SpectralGPUKernel.cu
Single big edit, idempotent via marker, backup .bak_sssburley.
"""

import argparse
import sys
from pathlib import Path


OLD = """static __forceinline__ __device__ float evalBSDF(
    const GPUMaterial& mat, float3 N, float3 V, float3 L, float lambda)
{
    float NdL=dot3(N,L); float NdV=dot3(N,V);
    if (NdL<=0.f||NdV<=0.f) return 0.f;
    float3 H=normalize3(make_float3(V.x+L.x,V.y+L.y,V.z+L.z));
    float NdH=dot3(N,H); float VdH=dot3(V,H);
    if (NdH<=0.f) return 0.f;
    float rough=fmaxf(0.01f,mat.roughness); float alpha=rough*rough;
    float baseR=spectralReflectance(mat,lambda);

    // Dispersion
    float ior = mat.ior;
    if (mat.abbeNumber > 0.f) ior = dispersedIOR(mat.ior, mat.abbeNumber, lambda);

    float F;
    if (mat.metallic > 0.5f && mat.metalType > 0) {
        // Measured metal: per-wavelength Fresnel tinted by base colour.
        // Matches SpectralBSDF's CPU path so gold/copper/silver etc.
        // look correct on GPU instead of identical Schlick silver.
        F = fresnelMetal(mat.metalType, lambda, VdH) * baseR;
    } else {
        float iorR=(ior-1.f)/(ior+1.f); float iorF0=iorR*iorR;
        float F0=iorF0+(baseR-iorF0)*mat.metallic;
        F=F0+(1.f-F0)*schlickW(VdH);
    }

    // Thin-film
    if (mat.thinFilmThickness > 0.f)
        F = thinFilmFresnel(F, ior, mat.thinFilmThickness, lambda, VdH);

    float D=ggxD(alpha,NdH); float G=smithG1(alpha,NdV)*smithG1(alpha,NdL);
    float spec=(D*G*F)/(4.f*NdV*NdL+1e-7f);
    float FD90=0.5f+2.f*VdH*VdH*rough;
    float diff=(1.f-mat.metallic)*mat.opacity*baseR*(1.f+(FD90-1.f)*schlickW(NdL))*(1.f+(FD90-1.f)*schlickW(NdV))/3.14159f;

    // Subsurface scattering (wrap-diffuse approximation).
    // The CPU path random-walks 16 steps per pixel; here we approximate
    // with wavelength-tinted wrap-around diffuse -- visually close for
    // skin/wax/marble at typical sssRadius values, zero cost when
    // sssRadius is 0.
    float sss = 0.f;
    if (mat.sssRadius > 0.f) {
        float sssSum = mat.sssColor.x + mat.sssColor.y + mat.sssColor.z;
        if (sssSum > 0.001f) {
            float rB = spectralGauss(lambda, 630.f, 30.f);
            float gB = spectralGauss(lambda, 532.f, 30.f);
            float bB = spectralGauss(lambda, 460.f, 25.f);
            float scatterAtLambda = mat.sssColor.x*rB + mat.sssColor.y*gB + mat.sssColor.z*bB;
            scatterAtLambda = fmaxf(0.01f, scatterAtLambda);
            float wrap = fminf(0.5f, mat.sssRadius * 0.5f);
            float wrappedNdL = (NdL + wrap) / ((1.f + wrap) * (1.f + wrap));
            if (wrappedNdL > 0.f) {
                float sssStrength = fminf(1.f, mat.sssRadius);
                sss = (1.f - mat.metallic) * mat.opacity * baseR
                    * scatterAtLambda * wrappedNdL * sssStrength / 3.14159f;
            }
        }
    }

    // Clearcoat (Disney 2012 / UsdPreviewSurface). Mirrors the CPU path
    // in SpectralBSDF::Evaluate. F0 fixed at 0.04 (IOR=1.5), Smith G
    // alpha fixed at 0.25 per Disney (keeps coat bright at grazing).
    float clearcoatLobe = 0.f;
    float clearcoatAttn = 1.f;
    if (mat.clearcoat > 0.f) {
        float Fc = 0.04f + (1.f - 0.04f) * schlickW(VdH);
        float ccRough = fmaxf(0.01f, mat.clearcoatRoughness);
        float ccAlpha = ccRough * ccRough;
        float Dc = ggxD(ccAlpha, NdH);
        float Gc = smithG1(0.25f, NdV) * smithG1(0.25f, NdL);
        clearcoatLobe = (Dc * Gc * Fc) / (4.f * NdV * NdL + 1e-7f);
        clearcoatLobe *= mat.clearcoat;
        clearcoatAttn = 1.f - Fc * mat.clearcoat;
    }

    // Base (diff + spec) and sss are attenuated by the coat; the coat
    // adds its specular on top. When clearcoat==0 this reduces exactly
    // to the pre-clearcoat formula (attn=1, lobe=0).
    return ((diff + spec) * NdL + sss) * clearcoatAttn + clearcoatLobe * NdL;
}
"""

NEW = """static __forceinline__ __device__ float evalBSDF(
    const GPUMaterial& mat, float3 N, float3 V, float3 L, float lambda)
{
    float NdL=dot3(N,L); float NdV=dot3(N,V);
    // Camera looking at backside of a single-sided surface still gets
    // nothing. For NdL<=0 we may still have SSS (back-lit thin-part
    // glow), so let that case fall through when sssRadius>0.
    if (NdV<=0.f) return 0.f;
    if (NdL<=0.f && mat.sssRadius<=0.f) return 0.f;
    float3 H=normalize3(make_float3(V.x+L.x,V.y+L.y,V.z+L.z));
    float NdH=dot3(N,H); float VdH=dot3(V,H);
    // NdH<=0 short-circuits diff/spec/clearcoat but not SSS; gated below.
    float rough=fmaxf(0.01f,mat.roughness); float alpha=rough*rough;
    float baseR=spectralReflectance(mat,lambda);

    // Dispersion
    float ior = mat.ior;
    if (mat.abbeNumber > 0.f) ior = dispersedIOR(mat.ior, mat.abbeNumber, lambda);

    // Diffuse + specular + clearcoat only for front-lit (NdL>0, NdH>0)
    // geometry. Back-lit hits fall through to the SSS block for the
    // thin-part glow (which needs NdL<=0 to fire).
    float diff = 0.f, spec = 0.f;
    float clearcoatLobe = 0.f;
    float clearcoatAttn = 1.f;
    if (NdL > 0.f && NdH > 0.f) {
        float F;
        if (mat.metallic > 0.5f && mat.metalType > 0) {
            // Measured metal: per-wavelength Fresnel tinted by base colour.
            // Matches SpectralBSDF's CPU path so gold/copper/silver etc.
            // look correct on GPU instead of identical Schlick silver.
            F = fresnelMetal(mat.metalType, lambda, VdH) * baseR;
        } else {
            float iorR=(ior-1.f)/(ior+1.f); float iorF0=iorR*iorR;
            float F0=iorF0+(baseR-iorF0)*mat.metallic;
            F=F0+(1.f-F0)*schlickW(VdH);
        }

        // Thin-film
        if (mat.thinFilmThickness > 0.f)
            F = thinFilmFresnel(F, ior, mat.thinFilmThickness, lambda, VdH);

        float D=ggxD(alpha,NdH); float G=smithG1(alpha,NdV)*smithG1(alpha,NdL);
        spec=(D*G*F)/(4.f*NdV*NdL+1e-7f);
        float FD90=0.5f+2.f*VdH*VdH*rough;
        diff=(1.f-mat.metallic)*mat.opacity*baseR*(1.f+(FD90-1.f)*schlickW(NdL))*(1.f+(FD90-1.f)*schlickW(NdV))/3.14159f;

        // Clearcoat (Disney 2012 / UsdPreviewSurface). F0 fixed at 0.04
        // (IOR=1.5), Smith G alpha fixed at 0.25 per Disney (keeps coat
        // bright at grazing).
        if (mat.clearcoat > 0.f) {
            float Fc = 0.04f + (1.f - 0.04f) * schlickW(VdH);
            float ccRough = fmaxf(0.01f, mat.clearcoatRoughness);
            float ccAlpha = ccRough * ccRough;
            float Dc = ggxD(ccAlpha, NdH);
            float Gc = smithG1(0.25f, NdV) * smithG1(0.25f, NdL);
            clearcoatLobe = (Dc * Gc * Fc) / (4.f * NdV * NdL + 1e-7f);
            clearcoatLobe *= mat.clearcoat;
            clearcoatAttn = 1.f - Fc * mat.clearcoat;
        }
    }

    // Subsurface scattering (Christensen-Burley 2015 normalized
    // diffusion, simplified for single-shade evaluation).
    //
    // The CPU path random-walks 16 steps per pixel. This GPU version
    // approximates the scatter with two terms:
    //   (1) Front-lit wrap: Burley equal-sum-of-two-exponentials profile
    //       with wavelength-dependent depth d. Smooth terminator rolloff.
    //   (2) Back-lit forward-scatter: exp(-|NdL|*2/d) transmission for
    //       thin-part glow (ears-from-behind red, jade/wax internal light).
    //
    // Both use d = sssRadius * albedoAtLambda, so red scatters further
    // than blue (matches CPU mean-free-path behaviour). Zero cost when
    // sssRadius is 0. Refs: Christensen & Burley 2015, "Approximate
    // Reflectance Profiles for Efficient Subsurface Scattering".
    float sss = 0.f;
    if (mat.sssRadius > 0.f) {
        float sssSum = mat.sssColor.x + mat.sssColor.y + mat.sssColor.z;
        if (sssSum > 0.001f) {
            // Albedo at this wavelength (Gaussian basis to convert RGB
            // colour to a spectral weight). Matches CPU path weighting.
            float rB = spectralGauss(lambda, 630.f, 30.f);
            float gB = spectralGauss(lambda, 532.f, 30.f);
            float bB = spectralGauss(lambda, 460.f, 25.f);
            float A = mat.sssColor.x*rB + mat.sssColor.y*gB + mat.sssColor.z*bB;
            A = fmaxf(0.01f, A);

            // Scatter depth d (Burley 2015). Wavelength-dependent via
            // albedo: deeper penetration for wavelengths where sssColor
            // is high. Floor at 0.05 avoids division-blow-up.
            float d = fmaxf(0.05f, mat.sssRadius * A);

            // --- Component 1: Burley wrap near terminator (NdL > 0) ---
            float wrapDist = fmaxf(0.f, 1.f - NdL) * d;
            float profile = (expf(-wrapDist / d) + expf(-wrapDist / (3.f * d))) * 0.125f;
            float wrapNdL = fmaxf(0.f, (NdL + d * 0.25f) / (1.f + d * 0.25f));
            float frontLit = (NdL > 0.f) ? (wrapNdL * profile) : 0.f;

            // --- Component 2: back-lit forward-scatter (NdL <= 0) ---
            float backLit = fmaxf(0.f, -NdL);
            float forward = (NdL <= 0.f) ? (expf(-backLit * 2.f / d) * backLit * 0.5f) : 0.f;

            float sssMag = frontLit + forward;
            sss = (1.f - mat.metallic) * mat.opacity * baseR * A
                * sssMag / 3.14159f;
        }
    }

    // Base (diff + spec) is Lambert-modulated by NdL; SSS carries its
    // own angular term via the Burley profile. Clearcoat adds on top.
    // When clearcoat==0 this reduces exactly to the pre-clearcoat
    // formula. When NdL<=0 diff/spec/clearcoat are 0; only SSS
    // (back-lit glow) contributes.
    float NdLpos = fmaxf(0.f, NdL);
    return ((diff + spec) * NdLpos + sss) * clearcoatAttn + clearcoatLobe * NdLpos;
}
"""


EDIT = (
    "Replace evalBSDF with Burley diffusion SSS (clearcoat preserved)",
    OLD, NEW,
    "Christensen-Burley 2015 normalized"
)


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
    if marker in text: return text, R.ALREADY
    c = text.count(o)
    if c == 0: return text, R.NOT_FOUND
    if c > 1:  return text, R.AMBIGUOUS
    return text.replace(o, n, 1), R.APPLIED


def process(path, dry, force, bak):
    print(f"\n=== {path.name} ===")
    if not path.exists():
        print(f"  ERROR: missing: {path}"); return False
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text = original
    desc, a, b, marker = EDIT
    text, s = apply_edit(text, a, b, marker)
    mk = {R.APPLIED:"[+]", R.ALREADY:"[=]", R.NOT_FOUND:"[!]", R.AMBIGUOUS:"[?]"}[s]
    print(f"  {mk} {desc}: {s}")
    ok = s not in (R.NOT_FOUND, R.AMBIGUOUS)
    if text == original:
        print("  (no changes needed)"); return ok
    if not ok and not force:
        print("  SKIPPED WRITE"); return False
    if dry:
        print(f"  DRY RUN: {len(text)-len(original):+d} chars"); return ok
    ob = text.encode(); obk = original.encode()
    if uses_crlf:
        ob = ob.replace(b"\n", b"\r\n"); obk = obk.replace(b"\n", b"\r\n")
    bakp = path.with_suffix(path.suffix + bak)
    bakp.write_bytes(obk); path.write_bytes(ob)
    print(f"  wrote {path.name}; backup {bakp.name}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)
    ok = process(args.src / "SpectralGPUKernel.cu", args.dry_run, args.force, ".bak_sssburley")
    if not ok: sys.exit(1)
    print("\nRebuild. Then compare GPU vs CPU renders with:")
    print("  - skin preset  (organic -> skin)     -- lifelike terminator")
    print("  - jade preset  (organic -> jade)     -- internal glow on backlit")
    print("  - wax preset   (organic -> wax)      -- soft silhouette")
    print("  - any sssColor + sssRadius combo in custom mode")
    print("")
    print("Visual things to look for:")
    print("  - Smoother terminator rolloff (no hard shadow edge)")
    print("  - Red glow on backlit thin parts (ears, fingertips)")
    print("  - Wavelength-dependent scatter (red tint persists deeper)")
    print("  - sssRadius > 1.0 now actually increases scatter strength")


if __name__ == "__main__":
    main()
