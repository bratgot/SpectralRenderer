#!/usr/bin/env python3
"""
apply_roadmap_parking_notes.py -- add Known issues / parked work section
and today's session journal entry to ROADMAP.txt.

Specifically:
  - New "Known issues / parked work" section before "Session journal"
    documenting: viewport tumble perf on dense geometry, Nuke exit
    crash, SpectralSurface preset UI refresh (from earlier this session),
    SpectralDrafting pass-through pivot (from earlier this session)
  - New session journal entry for today (2026-04-20)

Non-mutating to prior content. Idempotent (detects already-applied via
presence of the new section header).

Files touched: ROADMAP.txt
CRLF-safe, backs up to .bak_roadmap.
"""

import argparse
import sys
from pathlib import Path


# Insert the new section right before "## Session journal"
# Anchor on that heading; we prepend the new section ahead of it so the
# journal stays last (scroll-convenient).

OLD = """## Session journal

One-line-per-session record of what happened. Add to the top. Serves as a
memory aid for "what did I work on last week?"

- **2026-04-19b**"""

NEW = """## Known issues / parked work

Bugs and rough edges that are real but not blocking. Documented here so
we don't re-diagnose them next session.

### Viewport tumble is slow on dense geometry

Scenes with >~50k triangles cause the 3D viewport to stutter during
tumble. Native Nuke viewport handles the same scenes smoothly, so this
is our code, not a driver / hardware limit.

Cause confirmed via tracers: `draw_handle` iterates all triangles in
four places per frame using OpenGL immediate mode (`glBegin(GL_TRIANGLES)`
+ `glVertex3f` per vertex). At 120k tris and 60fps that is ~21M
`glVertex3f` calls per second, which the driver cannot keep up with.
Sites:
  - Scene bounds for shadow-map lightVP (full iteration to compute AABB)
  - Shadow-map depth pass (the dominant single cost)
  - Reflection pre-pass (when `vp_geo_reflections` is on)
  - Main geometry draw

Things tried that did not fix it:
  - `vp_enable_shadows` knob added this session; default off. Gates only
    the shadow-map pass. Shrinks the problem but does not solve it --
    the other three sites still iterate 120k tris immediate-mode.
  - Part B of a VBO migration (`.bak_tumblefixB` backups -- now reverted
    in place). Migrated all four sites at once to `glDrawArrays`. Broke
    shading (geometry went near-black) without fixing perf. Too ambitious
    for one patch.

Infrastructure that stays in place for a future proper fix:
  - `_EnsureGeoVBO()` helper (see SpectralRenderIop.cpp)
  - `_glGeoVBO`, `_glGeoMeshRanges`, `_glSceneMin/Max`, `_glGeoVBODirty`
    members (see SpectralRenderIop.h)
  - `TumbleDiag:` tracers in `append`, `_validate`, `draw_handle`

Next attempt should migrate ONE site at a time (test, confirm, continue),
starting with the shadow-depth pass since it has no material uniforms,
normals, or UVs -- just positions. If that works, move to reflection,
then main draw. Watch out for GL client-array state bleed between the
immediate-mode light gizmos/handles and the VBO sites -- a reasonable
guess is that Part B's failure was client-state contamination.

Alternative: write a custom viewport path (VBO-only from the start,
own shader) instead of extending the existing ad-hoc draw_handle mess.
Bigger scope but cleaner result.

### Nuke crashes on exit

As of ~2026-04-19. Has not been triaged -- needs own session.

Likely culprits (in rough probability order):
  - Static `std::unordered_map` registries in the shader/geom ops
    (`SpectralSurfaceOp`, `SpectralWireframeOp`, `SpectralShadowCatcherOp`,
    `SpectralMeshPropertiesOp`) accessed during Nuke's Op teardown in
    an order-of-destruction-dependent way. Static-destruction order
    bugs are classic crash-on-exit offenders.
  - OptiX resources not released on plugin unload (pipeline, SBT,
    module, context, disk cache lock).
  - CUDA context outliving something it points to.

To triage:
  - Reproduce: does it crash on exit from a fresh empty .nk? From a .nk
    with SpectralRender nodes but never rendered? After one render? After
    many renders?
  - Compare: rename SpectralRender.dll to disable the plugin; does Nuke
    exit cleanly?
  - Stack: attach debugger, grab stack at crash, check whether it's our
    code or Nuke's (or a driver DLL).
  - Valgrind / ASAN if feasible.

### SpectralSurface preset UI refresh

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

### SpectralDrafting pass-through to upstream surface

Attempted mid-session: make SpectralDrafting a pass-through so it adds
wireframe overlay without clobbering the upstream material. Didn't
work -- Nuke/USG binds materials by the node wired into GeoBindMaterial,
not by what `createShaderGraph` returns. Design pivot proposed: convert
SpectralDrafting from ShaderOp to GeomOp (sits inline before
GeoBindMaterial, registers wireframe params per mesh, no material
creation). User did not respond to the pivot question -- work paused
at that decision point.

Code changes in place: `SpectralDrafting` has `test_input` and
`maximum_inputs=1` (see `.bak_draftingpass` backup). Does nothing
useful in current state; safe to leave until we decide to re-attempt.

---

## Session journal

One-line-per-session record of what happened. Add to the top. Serves as a
memory aid for "what did I work on last week?"

- **2026-04-20** -- GPU parity work: metalType (7-metal n,k table +
  conductor Fresnel) and SSS (wrap-diffuse approximation) ported to GPU
  kernel, both wired through host material upload. Diagnosed + resolved
  OptiX error 7001 silent-CPU-fallback that had been hiding on every
  render for the whole session (self-resolved on rebuild, probably struct
  ABI mismatch from metalType add). Investigated viewport tumble freeze
  on dense geo -- diagnosed as immediate-mode GL iteration of 120k tris
  four times per draw_handle. Attempted VBO migration (Part A
  infrastructure stays, Part B reverted after shading regression).
  Added `vp_enable_shadows` knob (default off) as partial mitigation.
  Tumble remains slow on dense geo -- parked for future session per
  "Known issues" above. Two other bugs added to Known issues:
  SpectralSurface preset UI refresh (abandoned mid-session), Nuke
  crash-on-exit (not yet triaged).
- **2026-04-19b**"""


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n):
    if n in text: return text, R.ALREADY
    c = text.count(o)
    if c == 0: return text, R.NOT_FOUND
    if c > 1:  return text, R.AMBIGUOUS
    return text.replace(o, n, 1), R.APPLIED


def process(path, dry, force):
    print(f"\n=== {path.name} ===")
    if not path.exists():
        print(f"  ERROR: missing: {path}"); return False
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text, s = apply_edit(original, OLD, NEW)
    mk = {R.APPLIED:"[+]", R.ALREADY:"[=]", R.NOT_FOUND:"[!]", R.AMBIGUOUS:"[?]"}[s]
    print(f"  {mk} Add Known issues section + 2026-04-20 journal entry: {s}")
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
    bakp = path.with_suffix(path.suffix + ".bak_roadmap")
    bakp.write_bytes(obk); path.write_bytes(ob)
    print(f"  wrote {path.name}; backup {bakp.name}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=Path("."),
                    help="Project root where ROADMAP.txt lives")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.root.is_dir():
        print(f"ERROR: --root not found: {args.root}", file=sys.stderr); sys.exit(1)
    ok = process(args.root / "ROADMAP.txt", args.dry_run, args.force)
    if not ok: sys.exit(1)
    print("\nROADMAP updated. Commit and push when ready.")


if __name__ == "__main__":
    main()
