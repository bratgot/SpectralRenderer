#!/usr/bin/env python3
"""
apply_remove_dead_mb_knobs.py -- remove three motion blur knobs that
are declared but never read anywhere in the codebase:

  - _motionSamples  (Int_knob "motion_samples"):
      Tooltip promised "Volume-velocity time samples across the shutter."
      Never referenced outside the declaration. Volume-velocity motion
      blur itself isn't implemented either.

  - _motionBlur     (Bool_knob "motion_blur"):
      Tooltip promised "Render volume motion blur from VDB velocity grids."
      Also never referenced. The entire divider "Volume velocity blur"
      goes with it since the knob is the only thing in it.

  - _aovMotion      (Bool_knob "aov_motion"):
      Tooltip promised "Screen-space motion vectors as vol_motion (RG)."
      Never wired to any AOV output.

Loading existing .nk files with these knob names will log an unknown-
knob warning once per open but otherwise behaves the same.

Also removes the whole "Volume velocity blur" divider+text+knob block
because without the knob the remaining UI copy is misleading.

Files touched: src/SpectralRenderIop.h, src/SpectralRenderIop.cpp
Idempotent, CRLF-safe, backs up to .bak_rmdeadmb.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  Header: remove three member declarations
# ============================================================================

H_AOV_OLD = """    bool  _aovVolDepth = false;
    bool  _aovMotion = false;
    bool  _aovDenoiseAlbedo = false;
"""

H_AOV_NEW = """    bool  _aovVolDepth = false;
    bool  _aovDenoiseAlbedo = false;
"""


H_MB_OLD = """    // Motion blur
    bool  _motionBlur = false;
    int   _shutterPreset = 1;  // 0=Start,1=Centre,2=End,3=Custom
    int   _motionSamples = 3;
"""

H_MB_NEW = """    // Motion blur
    int   _shutterPreset = 1;  // 0=Start,1=Centre,2=End,3=Custom
"""


# ============================================================================
#  Cpp fix 1: drop _motionSamples Int_knob + its tooltip
# ============================================================================

CPP_MS_OLD = """        Double_knob(f, &_shutterClose, "shutter_close", "close"); SetRange(f, 0, 1);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Shutter close time relative to frame, in frames.\\n"
                   "0.25 = one quarter frame after current (default, 180 shutter centred).\\n"
                   "0.5  = one half   frame after current (360 shutter centred, full blur).");
        Int_knob(f, &_motionSamples, "motion_samples", "samples"); SetRange(f, 2, 8);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Volume-velocity time samples across the shutter.\\n"
                   "2 = fast. 3 = good. 5 = smooth. 8 = overkill.\\n"
                   "Object motion blur doesn't use this -- it samples time\\n"
                   "once per spp, so raise the main spp for smoother blur.");
    }
"""

CPP_MS_NEW = """        Double_knob(f, &_shutterClose, "shutter_close", "close"); SetRange(f, 0, 1);
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Shutter close time relative to frame, in frames.\\n"
                   "0.25 = one quarter frame after current (default, 180 shutter centred).\\n"
                   "0.5  = one half   frame after current (360 shutter centred, full blur).");
    }
"""


# ============================================================================
#  Cpp fix 2: drop the whole Volume velocity blur divider + knob
# ============================================================================

CPP_VMB_OLD = """    Divider(f, "Volume velocity blur");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "VDB velocity grids (vel/v) drive volume sample motion across the shutter."
            "</font>"
        );
        Newline(f);
        Bool_knob(f, &_motionBlur, "motion_blur", "enable");
        Tooltip(f, "Render volume motion blur from VDB velocity grids.\\n"
                   "Offsets volume samples across the shutter interval.\\n"
                   "Requires a velocity grid to be loaded.\\n"
                   "\\n"
                   "This is NOT the same as object motion blur above --\\n"
                   "this is purely for volumes that carry per-voxel velocity data.");
    }

"""

CPP_VMB_NEW = """"""


# ============================================================================
#  Cpp fix 3: drop _aovMotion knob
# ============================================================================

CPP_AOV_OLD = """    Bool_knob(f, &_aovMotion, "aov_motion", "motion vectors");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Screen-space motion vectors as vol_motion (RG).\\n"
               "Pixels/frame displacement. Requires velocity grid.\\n"
               "Feed to VectorBlur or MotionBlur nodes in comp.");
"""

CPP_AOV_NEW = """"""


EDITS_H = [
    ("Remove _aovMotion member",                 H_AOV_OLD, H_AOV_NEW),
    ("Remove _motionBlur + _motionSamples",      H_MB_OLD,  H_MB_NEW),
]

EDITS_CPP = [
    ("Remove motion_samples Int_knob",           CPP_MS_OLD,  CPP_MS_NEW),
    ("Remove volume velocity blur divider+knob", CPP_VMB_OLD, CPP_VMB_NEW),
    ("Remove aov_motion Bool_knob",              CPP_AOV_OLD, CPP_AOV_NEW),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n):
    # Empty new text is a valid delete. Detect "already applied" by absence
    # of a distinctive substring from the OLD block.
    if n == "":
        # Pick a distinctive line from OLD to use as "present or not" marker.
        marker_candidates = [
            line.strip() for line in o.strip().splitlines()
            if line.strip() and len(line.strip()) > 15
        ]
        if not marker_candidates:
            return text, R.NOT_FOUND
        marker = marker_candidates[0]
        if marker not in text:
            return text, R.ALREADY
    else:
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
    bak = ".bak_rmdeadmb"
    ok = True
    ok &= process(args.src / "SpectralRenderIop.h",   EDITS_H,   args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralRenderIop.cpp", EDITS_CPP, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nDone. Rebuild. Existing .nk files referencing motion_samples,\n"
          "motion_blur, or aov_motion will log 'unknown knob' once on load\n"
          "but otherwise work normally.")


if __name__ == "__main__":
    main()
