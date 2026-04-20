#!/usr/bin/env python3
"""
apply_claudemd_shaderop_gotcha.py -- document the ShaderOp preset/push
gotcha in CLAUDE.md so future-us doesn't waste a diagnostic session
rediscovering it.

Adds a subsection to the Nuke plugin architecture area explaining:
  - Writing knobs programmatically needs member-first + return 1
  - Why set_value-only + fallthrough-to-parent eats the Op instance
  - The Volume vs (my first) Surface comparison as the canonical
    reference pattern

The insight is non-obvious because `return 1` is already mentioned in
CLAUDE.md in the hash-invalidation context, but that note is about a
DIFFERENT concern (hash propagation). The new gotcha is about Op
instance lifetime under ShaderOp default handling -- same two-word
mechanic, different reason.
"""

import argparse
import sys
from pathlib import Path


# Anchor: insert just before the "---" separator that closes the Nuke
# section, so the new gotcha sits with the other knob-related notes.
ANCHOR_OLD = """- `SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION)` for read-only display knobs.

---

## C++ / build gotchas
"""

ANCHOR_NEW = """- `SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION)` for read-only display knobs.

### Pushing values to knobs programmatically (presets, auto-populate)

When code inside a ShaderOp writes to its own knobs via `Knob::set_value`
-- typical cases: applying a preset, auto-computing a derived field,
restoring defaults -- follow the SpectralVolumeMaterial pattern exactly
or things break in non-obvious ways. The rules:

1. **Write the bound member DIRECTLY first, then call `set_value`.**
   ```cpp
   _diffuseColor[0] = diffuse[0];  // direct write first
   _diffuseColor[1] = diffuse[1];
   _diffuseColor[2] = diffuse[2];
   if (Knob* k = knob("diffuse_color")) {
       k->set_value(_diffuseColor[0], 0);  // now a no-op (bound matches)
       k->set_value(_diffuseColor[1], 1);
       k->set_value(_diffuseColor[2], 2);
   }
   ```
   This makes the `set_value` calls no-ops -- Nuke's change detection
   sees "bound value matches new value" and does not queue a deferred
   `knob_changed` callback for the pushed knob.
   Counter-intuitively the `set_value` is still necessary: it drives
   the panel-widget repaint. Without it the render output updates but
   the sliders visually stay on the old values.

2. **`return 1` after handling the originating knob, do not fall
   through to `ShaderOp::knob_changed(k)`.** For ShaderOp subclasses,
   Nuke's default handling can rebuild the Op when a knob in the panel
   changes under certain conditions. If your handler has just pushed
   ~20 values to knobs and then you let the default run, the Op gets
   destroyed + recreated, and subsequent `knob_changed` callbacks land
   on a fresh instance that never reaches your `_ApplyPreset` /
   auto-populate code. Symptom: "preset works the first time or two,
   then stops firing." Fix:
   ```cpp
   if (k->is("preset") && _spectralPreset != _lastAppliedPreset) {
       _ApplyPreset(_spectralPreset);
       _lastAppliedPreset = _spectralPreset;
       return 1;    // do NOT fall through to ShaderOp::knob_changed
   }
   return ShaderOp::knob_changed(k);  // fall through for all other knobs
   ```

3. **If the push-to-knobs order matters, go: members first -> set_value
   second.** Doing set_value first (member gets updated by the knob
   binding) also works *mechanically* but fires a deferred callback for
   every differing knob, which is what creates the callback cascade
   that triggers the Op rebuild in (2). Always do direct member write
   first to suppress the cascade.

The distinction vs the `return 1` note in the hash-invalidation section
above: that note is about whether Nuke re-hashes/re-renders. This one
is about whether the Op instance stays alive. Both reasons for returning
1 matter; they're independent.

Diagnostic tip: if presets / auto-populate code stops firing
`knob_changed` after the first pick, add a trace line at the top of
`knob_changed` that prints `this=%p` and the name of the firing knob.
If `this` changes between picks, the Op is being rebuilt -- go add
`return 1`.

Reference implementations:
- Good: `SpectralVolumeMaterial::knob_changed` (members-first + return 1)
- Bad (early 2026-04-20 attempt): `SpectralSurfaceOp::_ApplyPresetV2`
  in the pre-fix history (set_value-only + fall-through)

---

## C++ / build gotchas
"""


EDITS = [
    ("Add ShaderOp preset/push gotcha subsection",
     ANCHOR_OLD, ANCHOR_NEW,
     "### Pushing values to knobs programmatically (presets, auto-populate)"),
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
    ap.add_argument("--claudemd", type=Path, default=Path("CLAUDE.md"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.claudemd.exists():
        print(f"ERROR: --claudemd not found: {args.claudemd}", file=sys.stderr); sys.exit(1)
    bak = ".bak_shaderopgotcha"
    ok = process(args.claudemd, EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)


if __name__ == "__main__":
    main()
