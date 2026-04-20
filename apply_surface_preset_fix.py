#!/usr/bin/env python3
"""
apply_surface_preset_fix.py -- the real fix, matching VolumeMaterial
exactly.

Previous diag runs proved:
  - preset knob fires knob_changed correctly
  - _ApplyPreset runs to completion with spec unchanged
  - BUT after "ap push done + kc branch done", knob_changed STOPS
    firing for any future preset pick

Volume pattern (which works) does TWO things my rewrite got wrong:

  1. Writes MEMBERS FIRST, then calls set_value. This makes set_value
     a no-op (bound value already matches), so Nuke's change detection
     doesn't fire deferred callbacks for every pushed knob.

  2. Returns 1 after applying the preset. This tells Nuke "I handled
     this, don't propagate or process further." My rewrite falls
     through to return ShaderOp::knob_changed(k), which lets Nuke's
     default handling run and (apparently) results in the Op getting
     rebuilt or swapped out, so subsequent preset picks never reach
     the new instance.

This patch:
  - Adds member writes BEFORE the set_value push in _ApplyPreset
  - Adds `return 1` after _ApplyPreset call in knob_changed
  - Leaves the diag tracers in place (they're low-cost and useful)

Once this is confirmed working, a separate cleanup patch removes the
diag traces.
"""

import argparse
import sys
from pathlib import Path


# ============================================================================
#  Fix 1: knob_changed -- add `return 1` after preset application
# ============================================================================

KC_OLD = """    if (k->is("preset") && _spectralPreset != _lastAppliedPreset) {
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

KC_NEW = """    if (k->is("preset") && _spectralPreset != _lastAppliedPreset) {
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


# ============================================================================
#  Fix 2: _ApplyPreset push block -- write members BEFORE set_value so
#  Nuke sees "value unchanged" and skips callback storms.
# ============================================================================

PUSH_OLD = """    fprintf(stderr, "surf.diag: ap about to push -- this=%p &_sp=%p spec-before-push=%d\\n",
        (void*)this, (void*)&_spectralPreset, _spectralPreset);
    fprintf(stderr, "surf.diag: ap about to push -- diffuse_color knob ptr=%p, sss_color ptr=%p\\n",
        (void*)knob("diffuse_color"), (void*)knob("sss_color"));
    int _spBefore = _spectralPreset;
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

PUSH_NEW = """    fprintf(stderr, "surf.diag: ap about to push -- this=%p &_sp=%p spec-before-push=%d\\n",
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


# Also remove the "_metalType = metalType;" direct write inside the push
# block, since it's now done in the member-first block above.
METALTYPE_OLD = """    // _metalType has no knob widget -- direct member write.
    _metalType = metalType;
    setVec3("absorption_color",  absColor);"""

METALTYPE_NEW = """    // _metalType written in member-first block above.
    setVec3("absorption_color",  absColor);"""


EDITS = [
    ("knob_changed: return 1 after preset applied (match Volume)",
     KC_OLD, KC_NEW,
     "Matches SpectralVolumeMaterial -- return 1 tells Nuke"),
    ("_ApplyPreset push: write members FIRST, then set_value",
     PUSH_OLD, PUSH_NEW,
     "Fix: write MEMBERS FIRST, then set_value."),
    ("_ApplyPreset push: remove duplicate _metalType write",
     METALTYPE_OLD, METALTYPE_NEW,
     "// _metalType written in member-first block above."),
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
    bak = ".bak_surffix"
    ok = process(args.src / "SpectralSurfaceOp.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Test: pick aluminium -> wood -> jade -> porcelain -> etc.")
    print("Expected traces:")
    print("  - kc entry knob=preset fires EACH time you pick a new preset")
    print("  - kc BRANCH TAKEN + ap push done + kc branch done cycle each pick")
    print("  - Sliders actually update to the new values on each pick")
    print("If it works, a cleanup patch can remove the diag traces.")


if __name__ == "__main__":
    main()
