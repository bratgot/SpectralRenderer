#!/usr/bin/env python3
"""
apply_surface_preset_diag.py -- add diagnostic tracers to the preset
path so we can see exactly where it's breaking.

Traces written to stderr with prefix "surf.diag:". On Windows / Nuke,
stderr goes to the Terminal window (Help -> Show Terminal) or to the
Nuke log file.

Traces added:
  1. knob_changed entry        -> name of knob that fired + current preset values
  2. knob_changed guard result -> whether the preset branch was taken
  3. _ApplyPreset entry        -> preset index received
  4. After header/custom gate  -> which path was taken (custom/header/normal)
  5. Inside switch for jade    -> locals state (sanity)
  6. Before push-to-knobs      -> pointer validity for each target knob
  7. After push                 -> done marker

Idempotent via "surf.diag:" substring check.

NOTE: Once the bug is fixed, remove these traces by running the
apply_surface_preset_diag_remove.py script (not included yet -- just
delete the lines manually, or wait for a cleanup patch).
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  Trace 1+2: knob_changed -- log entry + guard result
# ============================================================================

KC_OLD = """int SpectralSurfaceOp::knob_changed(Knob* k)
{
    // Single master preset dropdown. Apply only when the value has
    // actually changed from the last applied one; this prevents
    // re-entrant panel-widget callbacks from re-applying the same
    // preset during set_value push.
    if (k->is("preset") && _spectralPreset != _lastAppliedPreset) {
        _ApplyPreset(_spectralPreset);
        _lastAppliedPreset = _spectralPreset;
    }

    // Always update registry + bump global version so SpectralRender detects changes
    RegisterParams();
    s_spectralSurfaceVersion.fetch_add(1);
    return ShaderOp::knob_changed(k);
}
"""

KC_NEW = """int SpectralSurfaceOp::knob_changed(Knob* k)
{
    // surf.diag: trace every knob_changed firing + guard state
    const char* _kname = k ? k->name().c_str() : "<null>";
    fprintf(stderr, "surf.diag: kc entry node=%s knob=%s spec=%d last=%d\\n",
        node_name().c_str(), _kname, _spectralPreset, _lastAppliedPreset);

    // Single master preset dropdown. Apply only when the value has
    // actually changed from the last applied one; this prevents
    // re-entrant panel-widget callbacks from re-applying the same
    // preset during set_value push.
    if (k->is("preset") && _spectralPreset != _lastAppliedPreset) {
        fprintf(stderr, "surf.diag: kc BRANCH TAKEN, calling _ApplyPreset(%d)\\n", _spectralPreset);
        _ApplyPreset(_spectralPreset);
        _lastAppliedPreset = _spectralPreset;
        fprintf(stderr, "surf.diag: kc branch done, last<-%d\\n", _lastAppliedPreset);
    } else if (k->is("preset")) {
        fprintf(stderr, "surf.diag: kc preset knob fired but spec==last (%d==%d), SKIP\\n",
            _spectralPreset, _lastAppliedPreset);
    }

    // Always update registry + bump global version so SpectralRender detects changes
    RegisterParams();
    s_spectralSurfaceVersion.fetch_add(1);
    return ShaderOp::knob_changed(k);
}
"""


# ============================================================================
#  Trace 3+4: _ApplyPreset entry + gate classification
# ============================================================================

AP_OLD = """void SpectralSurfaceOp::_ApplyPreset(int preset)
{
    if (preset == 0) return;  // custom -- don't change anything
    // Header entries (dividers in the enum list) -- do nothing
    if (preset == 1 || preset == 6 || preset == 11 || preset == 18 || preset == 22
        || preset == 26 || preset == 32 || preset == 38) return;

    // Locals: defaults for a "fresh custom material" (no advanced features).
"""

AP_NEW = """void SpectralSurfaceOp::_ApplyPreset(int preset)
{
    fprintf(stderr, "surf.diag: ap entry preset=%d\\n", preset);
    if (preset == 0) {
        fprintf(stderr, "surf.diag: ap early return (custom)\\n");
        return;  // custom -- don't change anything
    }
    // Header entries (dividers in the enum list) -- do nothing
    if (preset == 1 || preset == 6 || preset == 11 || preset == 18 || preset == 22
        || preset == 26 || preset == 32 || preset == 38) {
        fprintf(stderr, "surf.diag: ap early return (header entry %d)\\n", preset);
        return;
    }
    fprintf(stderr, "surf.diag: ap reached switch with preset=%d\\n", preset);

    // Locals: defaults for a "fresh custom material" (no advanced features).
"""


# ============================================================================
#  Trace 5: inside jade case (preset 17) -- known target for the bug
# ============================================================================

JADE_OLD = """        case 17: // jade
            diffuse[0]=0.15f; diffuse[1]=0.5f; diffuse[2]=0.2f;
            roughness=0.3f; ior=1.66f;
            sssCol[0]=0.2f; sssCol[1]=0.7f; sssCol[2]=0.25f; sssRad=0.3f;
            break;
"""

JADE_NEW = """        case 17: // jade
            diffuse[0]=0.15f; diffuse[1]=0.5f; diffuse[2]=0.2f;
            roughness=0.3f; ior=1.66f;
            sssCol[0]=0.2f; sssCol[1]=0.7f; sssCol[2]=0.25f; sssRad=0.3f;
            fprintf(stderr, "surf.diag: ap case JADE: diff=(%.2f,%.2f,%.2f) rough=%.2f ior=%.2f sssR=%.2f\\n",
                diffuse[0], diffuse[1], diffuse[2], roughness, ior, sssRad);
            break;
"""


# ============================================================================
#  Trace 6+7: push-to-knobs -- verify knob() lookup works
# ============================================================================

PUSH_OLD = """    // Flush locals to knobs via set_value. This is the key to the
    // refresh: members still hold the PREVIOUS value until set_value
    // runs, so Nuke's change detection fires correctly and the panel
    // refreshes. Mirrors SpectralVolumeMaterial's reliable pattern.
    auto setVec3 = [&](const char* name, const float v[3]) {
        if (Knob* k = knob(name)) {
            k->set_value(v[0], 0);
            k->set_value(v[1], 1);
            k->set_value(v[2], 2);
        }
    };
    auto setF = [&](const char* name, float v) {
        if (Knob* k = knob(name)) k->set_value(v);
    };
    setVec3("diffuse_color",     diffuse);
"""

PUSH_NEW = """    // Flush locals to knobs via set_value. This is the key to the
    // refresh: members still hold the PREVIOUS value until set_value
    // runs, so Nuke's change detection fires correctly and the panel
    // refreshes. Mirrors SpectralVolumeMaterial's reliable pattern.
    fprintf(stderr, "surf.diag: ap about to push -- diffuse_color knob ptr=%p, sss_color ptr=%p\\n",
        (void*)knob("diffuse_color"), (void*)knob("sss_color"));
    auto setVec3 = [&](const char* name, const float v[3]) {
        Knob* k = knob(name);
        if (k) {
            k->set_value(v[0], 0);
            k->set_value(v[1], 1);
            k->set_value(v[2], 2);
        } else {
            fprintf(stderr, "surf.diag: push setVec3 MISS name=%s\\n", name);
        }
    };
    auto setF = [&](const char* name, float v) {
        Knob* k = knob(name);
        if (k) { k->set_value(v); }
        else { fprintf(stderr, "surf.diag: push setF MISS name=%s\\n", name); }
    };
    setVec3("diffuse_color",     diffuse);
"""


# ============================================================================
#  Trace 7: end-of-function marker after the last setF call
# ============================================================================

END_OLD = """    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
}
"""

END_NEW = """    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
    fprintf(stderr, "surf.diag: ap push done\\n");
}
"""


EDITS = [
    ("Add <cstdio> include for fprintf",
     "#include <atomic>\n",
     "#include <atomic>\n#include <cstdio>  // fprintf for surf.diag traces\n",
     "#include <cstdio>  // fprintf for surf.diag traces"),
    ("knob_changed: add entry + guard-result traces",
     KC_OLD, KC_NEW,
     "surf.diag: kc entry"),
    ("_ApplyPreset: add entry + gate-classification traces",
     AP_OLD, AP_NEW,
     "surf.diag: ap entry"),
    ("_ApplyPreset: jade case sanity trace",
     JADE_OLD, JADE_NEW,
     "surf.diag: ap case JADE"),
    ("_ApplyPreset: push-to-knobs ptr validity + MISS traces",
     PUSH_OLD, PUSH_NEW,
     "surf.diag: ap about to push"),
    ("_ApplyPreset: end-of-function marker",
     END_OLD, END_NEW,
     "surf.diag: ap push done"),
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
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)
    bak = ".bak_surfdiag"
    ok = process(args.src / "SpectralSurfaceOp.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Open Nuke, open your .nk scene, pick any preset from")
    print("the SpectralSurface preset dropdown. Check the Nuke Terminal")
    print("(Help -> Show Terminal) for lines starting with 'surf.diag:'.")
    print("Copy/paste those lines back to continue debugging.")


if __name__ == "__main__":
    main()
