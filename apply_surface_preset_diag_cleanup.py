#!/usr/bin/env python3
"""
apply_surface_preset_diag_cleanup.py -- remove all surf.diag traces
now that the preset path is confirmed working.

Removes:
  - #include <cstdio> (only added for the traces)
  - All fprintf(stderr, "surf.diag: ...") lines
  - The _spBefore snapshot and mid-push change checks
  - The _spBefore reads inside the lambdas

Keeps intact:
  - return 1 after preset applied (the critical fix)
  - Member-first writes in the push block (the other critical fix)
  - All the actual preset logic
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  Remove <cstdio> include (only added for diag)
# ============================================================================

INCLUDE_OLD = """#include <atomic>
#include <cstdio>  // fprintf for surf.diag traces
"""
INCLUDE_NEW = """#include <atomic>
"""


# ============================================================================
#  Clean up knob_changed: strip diag fprintf, keep return 1 and logic
# ============================================================================

KC_OLD = """int SpectralSurfaceOp::knob_changed(Knob* k)
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
        // Matches SpectralVolumeMaterial -- return 1 tells Nuke we handled
        // this fully, don't propagate to default ShaderOp handling. Without
        // this the Op gets rebuilt mid-callback and subsequent preset picks
        // hit a different instance that never reaches our knob_changed.
        RegisterParams();
        s_spectralSurfaceVersion.fetch_add(1);
        return 1;
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

KC_NEW = """int SpectralSurfaceOp::knob_changed(Knob* k)
{
    // Single master preset dropdown. Apply only when the value has
    // actually changed from the last applied one; this prevents
    // re-entrant panel-widget callbacks from re-applying the same
    // preset during set_value push.
    //
    // CRITICAL: return 1 after preset apply, matching SpectralVolume-
    // Material. Without this Nuke's default ShaderOp handling rebuilds
    // the Op mid-callback and subsequent preset picks never reach the
    // current instance. Combined with member-first writes in the push
    // block (so set_value becomes a no-op that doesn't fire further
    // callbacks), this is what makes the preset panel behave reliably.
    if (k->is("preset") && _spectralPreset != _lastAppliedPreset) {
        _ApplyPreset(_spectralPreset);
        _lastAppliedPreset = _spectralPreset;
        RegisterParams();
        s_spectralSurfaceVersion.fetch_add(1);
        return 1;
    }

    // Always update registry + bump global version so SpectralRender detects changes
    RegisterParams();
    s_spectralSurfaceVersion.fetch_add(1);
    return ShaderOp::knob_changed(k);
}
"""


# ============================================================================
#  Clean up _ApplyPreset entry traces
# ============================================================================

AP_OLD = """void SpectralSurfaceOp::_ApplyPreset(int preset)
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

AP_NEW = """void SpectralSurfaceOp::_ApplyPreset(int preset)
{
    if (preset == 0) return;  // custom -- don't change anything
    // Header entries (dividers in the enum list) -- do nothing
    if (preset == 1 || preset == 6 || preset == 11 || preset == 18 || preset == 22
        || preset == 26 || preset == 32 || preset == 38) return;

    // Locals: defaults for a "fresh custom material" (no advanced features).
"""


# ============================================================================
#  Clean up jade case sanity trace
# ============================================================================

JADE_OLD = """        case 17: // jade
            diffuse[0]=0.15f; diffuse[1]=0.5f; diffuse[2]=0.2f;
            roughness=0.3f; ior=1.66f;
            sssCol[0]=0.2f; sssCol[1]=0.7f; sssCol[2]=0.25f; sssRad=0.3f;
            fprintf(stderr, "surf.diag: ap case JADE: diff=(%.2f,%.2f,%.2f) rough=%.2f ior=%.2f sssR=%.2f\\n",
                diffuse[0], diffuse[1], diffuse[2], roughness, ior, sssRad);
            break;
"""

JADE_NEW = """        case 17: // jade
            diffuse[0]=0.15f; diffuse[1]=0.5f; diffuse[2]=0.2f;
            roughness=0.3f; ior=1.66f;
            sssCol[0]=0.2f; sssCol[1]=0.7f; sssCol[2]=0.25f; sssRad=0.3f;
            break;
"""


# ============================================================================
#  Clean up push block: remove diag, keep member-first + lambdas
# ============================================================================

PUSH_OLD = """    fprintf(stderr, "surf.diag: ap about to push -- this=%p &_sp=%p spec-before-push=%d\\n",
        (void*)this, (void*)&_spectralPreset, _spectralPreset);
    fprintf(stderr, "surf.diag: ap about to push -- diffuse_color knob ptr=%p, sss_color ptr=%p\\n",
        (void*)knob("diffuse_color"), (void*)knob("sss_color"));
    int _spBefore = _spectralPreset;

    // Fix: write MEMBERS FIRST, then set_value. Volume does this to make
    // set_value a no-op (bound value matches before the call) which
    // suppresses Nuke's deferred change-notification callbacks. If we do
    // set_value first, Nuke fires knob_changed for every pushed knob
    // which is what's causing the Op instance swap.
    _diffuseColor[0] = diffuse[0];
    _diffuseColor[1] = diffuse[1];
    _diffuseColor[2] = diffuse[2];
    _metallic             = metallic;
    _roughness            = roughness;
    _ior                  = ior;
    _opacity              = opacity;
    _emissiveColor[0]     = emissive[0];
    _emissiveColor[1]     = emissive[1];
    _emissiveColor[2]     = emissive[2];
    _clearcoat            = clearcoat;
    _clearcoatRoughness   = clearRough;
    _abbeNumber           = abbe;
    _thinFilmThickness    = thinFilm;
    _metalType            = metalType;
    _absorptionColor[0]   = absColor[0];
    _absorptionColor[1]   = absColor[1];
    _absorptionColor[2]   = absColor[2];
    _absorptionDensity    = absDensity;
    _gratingSpacing       = gratingSp;
    _gratingStrength      = gratingStr;
    _fluorAbsorb          = flAbsorb;
    _fluorEmit            = flEmit;
    _fluorStrength        = flStrength;
    _sssColor[0]          = sssCol[0];
    _sssColor[1]          = sssCol[1];
    _sssColor[2]          = sssCol[2];
    _sssRadius            = sssRad;

    auto setVec3 = [&](const char* name, const float v[3]) {
        Knob* k = knob(name);
        if (k) {
            k->set_value(v[0], 0);
            k->set_value(v[1], 1);
            k->set_value(v[2], 2);
            if (_spectralPreset != _spBefore) {
                fprintf(stderr, "surf.diag: !! spec CHANGED during setVec3(%s): %d -> %d\\n",
                    name, _spBefore, _spectralPreset);
                _spBefore = _spectralPreset;
            }
        } else {
            fprintf(stderr, "surf.diag: push setVec3 MISS name=%s\\n", name);
        }
    };
    auto setF = [&](const char* name, float v) {
        Knob* k = knob(name);
        if (k) {
            k->set_value(v);
            if (_spectralPreset != _spBefore) {
                fprintf(stderr, "surf.diag: !! spec CHANGED during setF(%s): %d -> %d\\n",
                    name, _spBefore, _spectralPreset);
                _spBefore = _spectralPreset;
            }
        }
        else { fprintf(stderr, "surf.diag: push setF MISS name=%s\\n", name); }
    };"""

PUSH_NEW = """    // CRITICAL: write MEMBERS FIRST, then set_value. Volume does this to
    // make set_value a no-op (bound value matches before the call) which
    // suppresses Nuke's deferred change-notification callbacks. Without
    // this, each pushed knob fires a fresh knob_changed, Nuke ends up
    // rebuilding the Op mid-cascade, and subsequent preset picks hit a
    // stale instance that never reaches our handler.
    _diffuseColor[0] = diffuse[0];
    _diffuseColor[1] = diffuse[1];
    _diffuseColor[2] = diffuse[2];
    _metallic             = metallic;
    _roughness            = roughness;
    _ior                  = ior;
    _opacity              = opacity;
    _emissiveColor[0]     = emissive[0];
    _emissiveColor[1]     = emissive[1];
    _emissiveColor[2]     = emissive[2];
    _clearcoat            = clearcoat;
    _clearcoatRoughness   = clearRough;
    _abbeNumber           = abbe;
    _thinFilmThickness    = thinFilm;
    _metalType            = metalType;
    _absorptionColor[0]   = absColor[0];
    _absorptionColor[1]   = absColor[1];
    _absorptionColor[2]   = absColor[2];
    _absorptionDensity    = absDensity;
    _gratingSpacing       = gratingSp;
    _gratingStrength      = gratingStr;
    _fluorAbsorb          = flAbsorb;
    _fluorEmit            = flEmit;
    _fluorStrength        = flStrength;
    _sssColor[0]          = sssCol[0];
    _sssColor[1]          = sssCol[1];
    _sssColor[2]          = sssCol[2];
    _sssRadius            = sssRad;

    auto setVec3 = [&](const char* name, const float v[3]) {
        if (Knob* k = knob(name)) {
            k->set_value(v[0], 0);
            k->set_value(v[1], 1);
            k->set_value(v[2], 2);
        }
    };
    auto setF = [&](const char* name, float v) {
        if (Knob* k = knob(name)) k->set_value(v);
    };"""


# ============================================================================
#  Clean up push-done marker
# ============================================================================

END_OLD = """    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
    fprintf(stderr, "surf.diag: ap push done -- spec-after-push=%d (was %d at start)\\n",
        _spectralPreset, _spBefore);
}
"""

END_NEW = """    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
}
"""


EDITS = [
    ("Remove <cstdio> include (diag only)",
     INCLUDE_OLD, INCLUDE_NEW,
     None),  # marker = None means check for reverse-already via NEW not containing OLD
    ("knob_changed: remove diag traces, keep return 1 fix",
     KC_OLD, KC_NEW,
     None),
    ("_ApplyPreset: remove entry/gate traces",
     AP_OLD, AP_NEW,
     None),
    ("_ApplyPreset: remove jade case trace",
     JADE_OLD, JADE_NEW,
     None),
    ("_ApplyPreset: remove push diag, keep member-first + lambdas",
     PUSH_OLD, PUSH_NEW,
     None),
    ("_ApplyPreset: remove push-done trace",
     END_OLD, END_NEW,
     None),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
    # marker=None means: if OLD not in text, consider it already applied
    # (because we're REMOVING content, not adding)
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
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)
    bak = ".bak_surfclean"
    ok = process(args.src / "SpectralSurfaceOp.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. No functional change -- just strips the diag traces.")
    print("Presets should still work exactly as they do now. After this,")
    print("zero 'surf.diag:' lines will appear in the terminal.")


if __name__ == "__main__":
    main()
