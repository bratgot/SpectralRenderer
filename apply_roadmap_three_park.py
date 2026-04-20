#!/usr/bin/env python3
"""
apply_roadmap_three_park.py -- park three items in ROADMAP.txt from the
2026-04-20 audit & parity review session:

  (1) UI cleanup: the "(CPU)" suffix next to SSS on SpectralSurface now
      needs to come off -- GPU SSS is parity with CPU after the random-
      walk port.
  (2) Volume shader parity audit: analogous to the surface-shader audit
      just completed. Check every CPU volume param against the GPU one
      and file discrepancies.
  (3) SpectralSurface preset menu -- port the SpectralVolumeMaterial
      preset mechanism, which the user reports works perfectly, to
      SpectralSurface. The existing SpectralSurface presets work but
      are said to be less polished than the volume-material version.

Anchors on the doubleSided-intermittent entry (the last known-issues
bullet from apply_roadmap_ds_intermittent.py). If that patch didn't
land, this one will NOT FOUND -- run it first.

Idempotent via marker, CRLF-safe, backup .bak_threepark.
"""

import argparse
import sys
from pathlib import Path


OLD = """  Next action: add a per-material doubleSided tracer at kernel shade
  entry (cheap: one fprintf from host-side comparison of expected-vs-
  what-was-uploaded). Confirm matCheck XOR is actually being computed
  over the flipped flag. Only after that can we nail which of the three
  suspects is live.
"""

NEW = """  Next action: add a per-material doubleSided tracer at kernel shade
  entry (cheap: one fprintf from host-side comparison of expected-vs-
  what-was-uploaded). Confirm matCheck XOR is actually being computed
  over the flipped flag. Only after that can we nail which of the three
  suspects is live.
- **Remove "(CPU)" suffix from SSS knob on SpectralSurface** (new,
  2026-04-20). The knob was labelled "(CPU)" back when GPU SSS was a
  wrap-diffuse approximation rather than true subsurface scattering.
  After the random-walk port on 2026-04-20, GPU SSS now matches CPU
  pixel-for-pixel (16-step spectral random walk with BVH exit detection,
  same Gaussian basis albedo, wavelength-dependent mean free path).
  Audit SpectralSurfaceOp.cpp / SpectralSurfaceOp.h for any label,
  tooltip, or help string mentioning SSS being CPU-only and remove it.
- **Surface-shader CPU/GPU parity audit gaps** (new, 2026-04-20).
  Audit completed on 2026-04-20 found that GPU shade path matches CPU
  for base colour, metallic, roughness, IOR, opacity, Abbe dispersion,
  thin-film, measured metals, clearcoat, Beer-Lambert absorption,
  grating diffraction, fluorescence, subsurface scattering (random walk),
  texture blend, shadow catcher, and dielectric transmission. Four gaps
  remain. Fix each as a standalone follow-up: (a) roughnessTexId -- CPU
  samples texture green channel to override roughness per-pixel; GPU
  ignores the field entirely. Needs GPUMaterial addition, host upload
  + matCheck coverage, kernel sample and override before BSDF eval.
  (b) metallicTexId -- CPU samples blue channel; GPU ignores. Same
  shape as (a). (c) Spectral emissive -- CPU adds mat.SpectralEmission
  (lambda) to radiance at every shade; GPU only handles emissive in
  scanlineCompat RGB path, spectral shade block has no emissive
  contribution. (d) Bump-map tangent frame -- CPU builds UV-gradient
  derived T/B from triangle vertex data; GPU uses an arbitrary
  "up x N" fallback. Bump pattern rotates differently per face. Fix
  needs triangle vertex positions plumbed through OptiX SBT
  (HitGroupData or per-triangle ptr in params).
- **Volume shader CPU/GPU parity audit needed** (new, 2026-04-20).
  Counterpart to the surface-shader audit. Scope: compare every field
  of SpectralVolumeMaterial / SpectralVolume against GPUVolume in
  SpectralGPUParams.h, and compare CPU shade path in SpectralIntegrator
  (volume section) against the kernel's volume shading. Known GPU
  fields that exist on GPUVolume: density/temp/flame tex, bbox,
  extinction/scattering/densityMult, phase (HG dual-lobe + Cornette-
  Shanks Mie), emission, powder, scatter colour, step size, jitter,
  shadow steps, shadow density, quality, adaptive step, render mode
  (Lit/Grey/Heat/Cool/BB/Expl), flame temperature range, core glow,
  Cherenkov, chromatic extinction, multiple-scattering approximation,
  grid noise, transform, grid mixer. Likely suspects before starting:
  the CPU path might have spectral-emission temperature blackbody
  handling that GPU lacks (or vice versa); volume-colour absorption
  across wavelengths may diverge; shadow-density sampling step counts
  may differ. File findings as a separate known-issues entry once the
  audit lists discrepancies.
- **Port SpectralVolumeMaterial preset menu to SpectralSurface** (new,
  2026-04-20). User reports the SpectralVolumeMaterial preset dropdown
  works perfectly -- one selection, all category knobs update, mutual
  reset of the other categories, tooltips clear. SpectralSurface's
  preset UI (the split-group polish from earlier this session) works
  but is different in mechanics. Take the VolumeMaterial approach
  across. Probably means: one master enum dropdown replacing the
  eight category dropdowns, driving _ApplyPresetV2; simplifies the UI
  and gives users a single entry point. Keep the existing eight
  category enums as "advanced" if backward-compatibility matters,
  otherwise replace them wholesale. Triage starts by reading
  SpectralVolumeMaterial.cpp's knobs() + knob_changed() to see the
  exact mechanism.
"""


EDIT = (
    "Park three new items (SSS CPU label, volume audit, preset port)",
    OLD, NEW,
    "Remove \"(CPU)\" suffix from SSS knob on SpectralSurface"
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
    ap.add_argument("--roadmap", type=Path, default=Path("ROADMAP.txt"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.roadmap.exists():
        print(f"ERROR: --roadmap not found: {args.roadmap}", file=sys.stderr); sys.exit(1)
    ok = process(args.roadmap, args.dry_run, args.force, ".bak_threepark")
    if not ok: sys.exit(1)


if __name__ == "__main__":
    main()
