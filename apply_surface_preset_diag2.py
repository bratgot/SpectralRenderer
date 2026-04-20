#!/usr/bin/env python3
"""
apply_surface_preset_diag2.py -- deeper tracers to find WHERE
_spectralPreset gets silently overwritten.

Previous diag session showed:
  kc entry knob=preset spec=10 last=0  (user picked aluminium)
  kc BRANCH TAKEN -> _ApplyPreset(10)
  ap push done
  kc branch done, last<-10
  kc entry knob=opacity spec=23 last=10   <-- spec SILENTLY CHANGED

Between "kc branch done" (where we'd just read spec=10 to write last) and
the next kc entry, _spectralPreset got clobbered to 23 without the preset
knob firing.

This patch adds traces at every point where _spectralPreset is read, plus
a read-back inside each setF/setVec3 in the push block. If we see spec
change value MID-push, we know it's the set_value callback itself
corrupting it. If it changes AFTER push, it's something external.

Additional tracers:
  - Before push block: snapshot this, spec, last, &_spectralPreset
  - After each setF call: re-read spec, log if changed
  - After setVec3: same
  - After push block, before returning from _ApplyPreset: final spec read
"""

import argparse
import sys
from pathlib import Path


# The push block where we instrument every setF/setVec3 callsite.
# Keep anchor tight - just the lambdas + first setVec3.
PUSH_BEFORE_OLD = """    // Flush locals to knobs via set_value. This is the key to the
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
    };"""

PUSH_BEFORE_NEW = """    // Flush locals to knobs via set_value. This is the key to the
    // refresh: members still hold the PREVIOUS value until set_value
    // runs, so Nuke's change detection fires correctly and the panel
    // refreshes. Mirrors SpectralVolumeMaterial's reliable pattern.
    fprintf(stderr, "surf.diag: ap about to push -- this=%p &_sp=%p spec-before-push=%d\\n",
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


END_OLD = """    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
    fprintf(stderr, "surf.diag: ap push done\\n");
}
"""

END_NEW = """    setVec3("sss_color",         sssCol);
    setF("sss_radius",           sssRad);
    fprintf(stderr, "surf.diag: ap push done -- spec-after-push=%d (was %d at start)\\n",
        _spectralPreset, _spBefore);
}
"""


EDITS = [
    ("Push block: snapshot _spBefore + check after each set_value",
     PUSH_BEFORE_OLD, PUSH_BEFORE_NEW,
     "int _spBefore = _spectralPreset;"),
    ("End-of-function marker: report final spec vs _spBefore",
     END_OLD, END_NEW,
     "ap push done -- spec-after-push"),
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
    bak = ".bak_surfdiag2"
    ok = process(args.src / "SpectralSurfaceOp.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nRebuild. Repeat the test: pick aluminium, then look at the")
    print("traces. We're looking for two things:")
    print("  1. Does 'this=' and '&_sp=' stay the same across presets?")
    print("     (Different values = Nuke is copy-constructing the Op)")
    print("  2. When does spec value change? During push (a setF logs")
    print("     'spec CHANGED') or after (ap push done shows final != start)?")


if __name__ == "__main__":
    main()
