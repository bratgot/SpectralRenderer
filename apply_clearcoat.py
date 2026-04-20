#!/usr/bin/env python3
"""
apply_clearcoat.py -- PHASE 3 of CPU/GPU surface parity.

Implement a Disney/UsdPreviewSurface-style clearcoat lobe on both CPU
and GPU paths. Fields (`clearcoat`, `clearcoatRoughness`) already exist
on SpectralMaterial, they plumb through from SpectralSurface knobs +
UsdPreviewSurface reads, and presets (car, lacquered toy, etc.) set
them to meaningful values -- but nothing was reading them to actually
shade with. This patch closes that.

Lobe formula (Disney 2012 / UsdPreviewSurface):
  - IOR fixed at 1.5 -> F0 = 0.04
  - Fresnel-Schlick
  - GGX D with alpha = clearcoatRoughness^2
  - Smith G with fixed alpha_g = 0.25 (Disney's choice -- avoids
    overly-dark clearcoat at grazing angles)
  - spec_c = D_c * G_c * F_c / (4 * NdV * NdL)
  - Added on top of base; base scaled by (1 - F_c * clearcoat) so we
    don't create energy (the coat "steals" light that would have hit
    the base).

Sampling: no separate clearcoat sampler for this first landing.
The existing GGX specular sampler produces rays near the specular
direction, which catches most of the clearcoat contribution at a
small variance cost. Multi-lobe MIS is a follow-up if noise becomes
an issue.

Files touched:
  src/SpectralBSDF.h                (CPU: add clearcoat to Evaluate)
  src/SpectralGPUParams.h           (GPU struct: add fields)
  src/SpectralGPU.cpp               (host: copy fields at 2 sites)
  src/SpectralGPUKernel.cu          (GPU: add clearcoat to evalBSDF)

Idempotent, CRLF-safe, backs up to .bak_clearcoat.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  SpectralBSDF.h -- CPU clearcoat in Evaluate
# ============================================================================
# Insert before the final `return (diffuse + specular) * NdotL;` line.
# Also scale the base terms by (1 - F_c * clearcoat).

BSDF_OLD = """        // Metals have no diffuse — only specular with coloured F0
        // Transparent materials (opacity < 1) reduce diffuse proportionally
        float diffuse = (1.f - mat.metallic) * mat.opacity * baseRefl * diffuseFactor / 3.14159f;

        return (diffuse + specular) * NdotL;
    }
"""

BSDF_NEW = """        // Metals have no diffuse — only specular with coloured F0
        // Transparent materials (opacity < 1) reduce diffuse proportionally
        float diffuse = (1.f - mat.metallic) * mat.opacity * baseRefl * diffuseFactor / 3.14159f;

        // ----------------------------------------------------------
        // Clearcoat (Disney 2012 / UsdPreviewSurface)
        //
        //   A thin dielectric layer on top of the base material.
        //   Fixed IOR = 1.5 (F0 = 0.04), fixed Smith G alpha = 0.25
        //   (Disney's choice -- keeps the coat bright at grazing
        //   rather than vanishing, which is what we want for car
        //   paint / lacquer). Own GGX roughness (clearcoatRoughness).
        //
        //   Added on top of (diffuse + specular). To keep energy
        //   honest, the base is scaled by (1 - F_c * clearcoat):
        //   the coat takes a fraction of the incoming light.
        // ----------------------------------------------------------
        float clearcoatLobe = 0.f;
        float clearcoatAttn = 1.f;
        if (mat.clearcoat > 0.f) {
            const float ccF0 = 0.04f;   // Fresnel at normal, IOR 1.5
            float Fc = ccF0 + (1.f - ccF0) * _SchlickWeight(VdotH);
            float ccRough = std::max(0.01f, mat.clearcoatRoughness);
            float ccAlpha = ccRough * ccRough;
            float Dc = _GGX_D(ccAlpha, NdotH);
            // Disney uses a Smith G with alpha = 0.25 regardless of
            // clearcoat roughness -- intentional, prevents the coat
            // disappearing at grazing angles.
            float Gc = _Smith_G(0.25f, NdotV, NdotL);
            clearcoatLobe = (Dc * Gc * Fc) / (4.f * NdotV * NdotL + 1e-7f);
            clearcoatLobe *= mat.clearcoat;
            clearcoatAttn = 1.f - Fc * mat.clearcoat;
        }

        return ((diffuse + specular) * clearcoatAttn + clearcoatLobe) * NdotL;
    }
"""


# ============================================================================
#  SpectralGPUParams.h -- add clearcoat fields to GPUMaterial
# ============================================================================

PARAMS_OLD = """    // Subsurface scattering: the kernel uses a wrap-diffuse approximation
    // because porting the CPU random walk (16 steps with ray-exit tests)
    // would thrash memory per-pixel. sssColor drives wavelength-dependent
    // scatter; sssRadius drives overall strength and wrap width.
    float3 sssColor;
    float  sssRadius;
};
"""

PARAMS_NEW = """    // Subsurface scattering: the kernel uses a wrap-diffuse approximation
    // because porting the CPU random walk (16 steps with ray-exit tests)
    // would thrash memory per-pixel. sssColor drives wavelength-dependent
    // scatter; sssRadius drives overall strength and wrap width.
    float3 sssColor;
    float  sssRadius;
    // Clearcoat (Disney 2012 / UsdPreviewSurface). Thin dielectric layer
    // on top of the base material. IOR fixed at 1.5 (F0 = 0.04) in the
    // kernel. `clearcoat` scales the lobe strength [0, 1]; when 0 the
    // lobe and attenuation skip entirely.
    float  clearcoat;
    float  clearcoatRoughness;
};
"""


# ============================================================================
#  SpectralGPU.cpp -- copy fields at both upload sites
# ============================================================================

GPU1_OLD = """                gpuMats[i].sssColor        = make_float3(mats[i].sssColor[0],
                                                         mats[i].sssColor[1],
                                                         mats[i].sssColor[2]);
                gpuMats[i].sssRadius       = mats[i].sssRadius;
            }
"""

GPU1_NEW = """                gpuMats[i].sssColor        = make_float3(mats[i].sssColor[0],
                                                         mats[i].sssColor[1],
                                                         mats[i].sssColor[2]);
                gpuMats[i].sssRadius       = mats[i].sssRadius;
                gpuMats[i].clearcoat          = mats[i].clearcoat;
                gpuMats[i].clearcoatRoughness = mats[i].clearcoatRoughness;
            }
"""


GPU2_OLD = """            gpuMats[i].sssColor        = make_float3(mats[i].sssColor[0],
                                                     mats[i].sssColor[1],
                                                     mats[i].sssColor[2]);
            gpuMats[i].sssRadius       = mats[i].sssRadius;
        }
"""

GPU2_NEW = """            gpuMats[i].sssColor        = make_float3(mats[i].sssColor[0],
                                                     mats[i].sssColor[1],
                                                     mats[i].sssColor[2]);
            gpuMats[i].sssRadius       = mats[i].sssRadius;
            gpuMats[i].clearcoat          = mats[i].clearcoat;
            gpuMats[i].clearcoatRoughness = mats[i].clearcoatRoughness;
        }
"""


# ============================================================================
#  SpectralGPUKernel.cu -- add clearcoat to evalBSDF, mirroring CPU
# ============================================================================
# Replace the final `return (diff + spec) * NdL + sss;` with the full
# clearcoat-modulated return. Also need the SSS remain after clearcoat
# attenuation -- conceptually SSS is subsurface, so a coat on top should
# also attenuate it. Matching the CPU "multiply base by (1-Fc*cc)".

KERNEL_OLD = """    // SSS is added on top of the direct diffuse with NdL factor applied
    // here. For non-SSS (radius==0) this is unchanged from before.
    return (diff + spec) * NdL + sss;
}
"""

KERNEL_NEW = """    // Clearcoat (Disney 2012 / UsdPreviewSurface). Mirrors the CPU path
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


EDITS_BSDF   = [("CPU BSDF: clearcoat lobe + base attenuation",   BSDF_OLD,   BSDF_NEW)]
EDITS_PARAMS = [("GPUMaterial: add clearcoat fields",             PARAMS_OLD, PARAMS_NEW)]
EDITS_GPU    = [
    ("Host upload site 1: copy clearcoat fields",                 GPU1_OLD,   GPU1_NEW),
    ("Host upload site 2: copy clearcoat fields",                 GPU2_OLD,   GPU2_NEW),
]
EDITS_KERNEL = [("Kernel evalBSDF: clearcoat lobe + attenuation", KERNEL_OLD, KERNEL_NEW)]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n):
    if n in text and o not in text: return text, R.ALREADY
    c = text.count(o)
    if c == 0: return text, R.NOT_FOUND
    if c > 1:  return text, R.AMBIGUOUS
    return text.replace(o, n, 1), R.APPLIED


def process(path, edits, dry, force, bak):
    print(f"\n=== {path.name} ===")
    if not path.exists():
        print(f"  ERROR: missing: {path}"); return False
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text = original; ok = True
    for desc, a, b in edits:
        text, s = apply_edit(text, a, b)
        mk = {R.APPLIED:"[+]", R.ALREADY:"[=]", R.NOT_FOUND:"[!]", R.AMBIGUOUS:"[?]"}[s]
        print(f"  {mk} {desc}: {s}")
        if s in (R.NOT_FOUND, R.AMBIGUOUS): ok = False
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
    bak = ".bak_clearcoat"
    ok = True
    ok &= process(args.src / "SpectralBSDF.h",       EDITS_BSDF,   args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPUParams.h",  EDITS_PARAMS, args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPU.cpp",      EDITS_GPU,    args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralGPUKernel.cu", EDITS_KERNEL, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nPhase 3 applied. Close Nuke, rebuild, reopen.")
    print("Test: SpectralSurface presets 'car paint' / 'lacquered toy' /\n"
          "'plastic' should now show a second glossy specular lobe over\n"
          "the base material. Crank clearcoat=1, clearcoatRoughness=0.02 on\n"
          "any matte/rough surface -- should look like it just got waxed.")


if __name__ == "__main__":
    main()
