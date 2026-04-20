#!/usr/bin/env python3
"""
apply_roadmap_preset_fix.py -- document the preset UI fix in ROADMAP.

Updates:
  1. Replace the stale "SpectralSurface preset UI refresh" parked
     section (which has a WRONG root cause) with a RESOLVED entry
     showing the actual mechanism and fix.
  2. Add a session journal entry for 2026-04-20.

The parked section had incorrect diagnosis ("Nuke's change-detection
compares bound variable to set_value argument, so member-first writes
make set_value a no-op and skip widget update"). The opposite was
actually true: member-first makes set_value a no-op which SUPPRESSES
the callback storm. The real bug was the missing `return 1` in
knob_changed, which caused Op rebuild mid-callback.
"""

import argparse
import sys
from pathlib import Path


OLD_SECTION = """### SpectralSurface preset UI refresh

Preset dropdown updates the bound `_spectralPreset` member but sliders
on the panel do not visually redraw to match the new values. Rendered
material IS correct; the UI just looks stale.

Tried this session: `asapUpdate()`, per-knob `Knob::changed()`,
`updateUI(outputContext())`. None refreshed the sliders.

Root cause: Nuke's change-detection compares the bound variable's
CURRENT value to the `set_value` argument. We write the members
directly in the `_ApplyPreset` switch before calling `set_value`,
so Nuke sees "same value, nothing to invalidate" and skips the widget
update.

Proper fix (for a dedicated session -- ~300 mechanical lines):
  - Restructure `_ApplyPreset` so the switch populates LOCAL temporaries
  - All writes to members happen through `set_value` only
  - Nuke sees "old->new, change" and invalidates the widget correctly

Currently-applied session code that can stay (harmless) or be backed
out (cosmetic):
  - Drift detector in `knob_changed` (catches preset changes on next
    interaction -- works correctly for render, just not UI)
  - Per-knob `Knob::changed()` calls in `_ApplyPreset`
  - `updateUI(outputContext())` call at end of `_ApplyPreset`
  - Back up suffixes: `.bak_presetreal`, `.bak_asap`, `.bak_changed`,
    `.bak_updateui`, `.bak_updateuifix`
"""


NEW_SECTION = """### SpectralSurface preset UI refresh -- RESOLVED 2026-04-20

Was: picking a preset from the dropdown would update the rendered
material but sliders on the panel wouldn't redraw. Variations: presets
worked for the first pick or two then silently broke, especially jade
and spectral-category items. Symptom was NOT "bound member didn't
change" -- the render output showed the new preset values correctly --
but the panel widgets stuck on old values AND after a few picks the
preset knob stopped even firing knob_changed.

The earlier (wrong) diagnosis assumed Nuke's change detection was
skipping widget invalidation when members were pre-written. That was
BACKWARDS. The truth came out via diagnostic tracers: after the first
preset pick, subsequent picks fired knob_changed on a DIFFERENT Op
instance than the one displayed. The Op got rebuilt mid-callback.

Actual root cause: two things the rewrite had wrong vs SpectralVolume-
Material (which always worked reliably):

  1. Missing `return 1;` after preset application. Without it,
     knob_changed falls through to `ShaderOp::knob_changed(k)` which
     runs Nuke's default handling. For reasons specific to Nuke 17's
     ShaderOp machinery, that default handling rebuilds the Op when
     a ShaderOp's knob set changes, which invalidates `this` and
     routes future callbacks to a fresh instance.

  2. Writing members via `set_value` only, without preceding direct
     member assignment. Every `set_value` where the bound value
     differs fires a deferred `knob_changed` for that pushed knob.
     When we push ~20 knobs in a row this cascades, and Nuke's
     instance swap happens somewhere in the cascade.

Volume pattern (which works) inverts both:
  - member = local; (direct write first)
  - if (Knob* k = knob(...)) k->set_value(v);  (now a no-op, suppresses
    the callback storm because Nuke sees bound value already matches)
  - return 1;  (after preset applied, don't fall through)

Fix applied in `apply_surface_preset_fix.py` (2026-04-20) plus unified
master dropdown UI from `apply_surface_preset_unify.py`. Diagnostic
tracers and cleanup in `apply_surface_preset_diag*.py`.

Lesson for future Nuke plugin work:
  - ShaderOp::knob_changed path may rebuild the Op under default
    handling. Always `return 1` when you've fully handled a knob.
  - For panel-pushing code (presets, auto-populate, etc.), write the
    member directly FIRST then call set_value -- Volume's pattern.
    set_value as the ONLY write path triggers callback storms that
    can end up swapping the Op out from under you.
  - Diagnostic tracers with `this=%p` snapshot on knob_changed entry
    are invaluable -- if the pointer changes mid-session, you're
    chasing an instance-lifetime issue, not a value-handling one.

Associated backups (.bak_surfunify, .bak_surfdiag, .bak_surfdiag2,
.bak_surffix, .bak_surfclean) can all be discarded once confirmed
stable.
"""


JOURNAL_OLD = """## Session journal
"""

JOURNAL_NEW = """## Session journal

- **2026-04-20 (late)** -- SpectralSurface preset UI bug fixed after
  a long diagnostic chase. User reported "presets stop working after
  a few selections, especially jade and spectral menus." Multiple
  wrong attempts (re-entry guards, mutual-reset logic, V2 local-first
  design) preceded the right diagnosis. Diagnostic tracers with
  `this=%p` snapshot on every knob_changed made the Op-instance swap
  visible. Fix: match SpectralVolumeMaterial exactly -- member-first
  writes in push block + `return 1` in knob_changed after apply. Also
  unified 8-category preset dropdowns into one master enum (simpler
  UX, fewer edge cases). See "SpectralSurface preset UI refresh --
  RESOLVED" for full writeup.

"""


EDITS = [
    ("Replace stale preset-UI-refresh section with RESOLVED writeup",
     OLD_SECTION, NEW_SECTION,
     "### SpectralSurface preset UI refresh -- RESOLVED 2026-04-20"),
    ("Add session journal entry for the preset fix",
     JOURNAL_OLD, JOURNAL_NEW,
     "2026-04-20 (late)** -- SpectralSurface preset UI bug fixed"),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
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
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.roadmap.exists():
        print(f"ERROR: --roadmap not found: {args.roadmap}", file=sys.stderr); sys.exit(1)
    bak = ".bak_presetresolved"
    ok = process(args.roadmap, EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)


if __name__ == "__main__":
    main()
