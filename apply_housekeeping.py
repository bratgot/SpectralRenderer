#!/usr/bin/env python3
"""
apply_housekeeping.py -- close out two ROADMAP entries that are now
fixed, and fix the subdiv-level knob range mismatch.

Scope (3 edits total):

  ROADMAP.txt:
    1. Remove "Remove '(CPU)' suffix from SSS knob on SpectralSurface"
       entry. Fixed 2026-04-20 by apply_sss_label_cleanup.
    2. Remove "Port SpectralVolumeMaterial preset menu to SpectralSurface"
       entry. Fixed 2026-04-20 by apply_surface_preset_unify +
       apply_surface_preset_fix.

  SpectralMeshPropertiesOp.cpp:
    3. SetRange(f, 0, 6) -> SetRange(f, 0, 4) on subdiv_level knob.
       OpenSubdiv clamps internally to [1,4], so 5 and 6 on the slider
       did nothing. Tooltip also cleaned: "4+ = very high" was
       unreachable, replaced with "4 = maximum" note.

No functional renderer changes -- just UI range alignment + roadmap
hygiene.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  ROADMAP: drop the two resolved entries
# ============================================================================

ROADMAP_SSS_OLD = """- **Remove "(CPU)" suffix from SSS knob on SpectralSurface** (new,
  2026-04-20). The knob was labelled "(CPU)" back when GPU SSS was a
  wrap-diffuse approximation rather than true subsurface scattering.
  After the random-walk port on 2026-04-20, GPU SSS now matches CPU
  pixel-for-pixel (16-step spectral random walk with BVH exit detection,
  same Gaussian basis albedo, wavelength-dependent mean free path).
  Audit SpectralSurfaceOp.cpp / SpectralSurfaceOp.h for any label,
  tooltip, or help string mentioning SSS being CPU-only and remove it.
"""

# Drop entirely -- no replacement.
ROADMAP_SSS_NEW = ""


ROADMAP_PRESET_OLD = """- **Port SpectralVolumeMaterial preset menu to SpectralSurface** (new,
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

# Drop entirely -- resolved in this session.
ROADMAP_PRESET_NEW = ""


# ============================================================================
#  SpectralMeshPropertiesOp: subdiv level range alignment
# ============================================================================

SUBDIV_OLD = """        Int_knob(f, &_subdivLevel, "subdiv_level", "level");
        SetRange(f, 0, 6);
        KnobModifiesAttribValues(f);
        Tooltip(f, "Subdivision iterations at render time.\\n"
                   "0 = use SpectralRender node's global setting\\n"
                   "1 = light (4x faces)\\n"
                   "2 = medium (16x faces) -- good default\\n"
                   "3 = high (64x faces) -- hero assets\\n"
                   "4+ = very high -- close-up detail\\n\\n"
                   "Each level quadruples the face count.");
"""

SUBDIV_NEW = """        Int_knob(f, &_subdivLevel, "subdiv_level", "level");
        SetRange(f, 0, 4);
        KnobModifiesAttribValues(f);
        Tooltip(f, "Subdivision iterations at render time.\\n"
                   "0 = use SpectralRender node's global setting\\n"
                   "1 = light (4x faces)\\n"
                   "2 = medium (16x faces) -- good default\\n"
                   "3 = high (64x faces) -- hero assets\\n"
                   "4 = maximum (256x faces) -- close-up detail\\n\\n"
                   "Each level quadruples the face count. OpenSubdiv\\n"
                   "clamps internally to [1,4].");
"""


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
    # marker=None: "already applied" means OLD not in text (REMOVE edits)
    # marker=str: "already applied" means marker in text
    if marker is None:
        if o not in text:
            return text, R.ALREADY
    else:
        if marker in text: return text, R.ALREADY
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
    for desc, a, b, marker in edits:
        text, s = apply_edit(text, a, b, marker)
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
    ap.add_argument("--roadmap", type=Path, default=Path("ROADMAP.txt"))
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.roadmap.exists():
        print(f"ERROR: --roadmap not found: {args.roadmap}", file=sys.stderr); sys.exit(1)
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)

    bak = ".bak_housekeep"

    roadmap_edits = [
        ("Drop SSS (CPU) suffix entry (resolved 2026-04-20)",
         ROADMAP_SSS_OLD, ROADMAP_SSS_NEW, None),
        ("Drop VolumeMaterial->Surface preset port entry (resolved 2026-04-20)",
         ROADMAP_PRESET_OLD, ROADMAP_PRESET_NEW, None),
    ]
    src_edits = [
        ("Subdiv level knob range 0-6 -> 0-4 (internal clamp is [1,4])",
         SUBDIV_OLD, SUBDIV_NEW,
         "clamps internally to [1,4]"),
    ]

    ok = True
    ok &= process(args.roadmap, roadmap_edits, args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralMeshPropertiesOp.cpp", src_edits, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild (subdiv knob change only affects the UI panel).")
    print("No behavioural change -- level 5/6 never worked anyway.")


if __name__ == "__main__":
    main()
