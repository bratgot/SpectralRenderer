#!/usr/bin/env python3
"""
apply_roadmap_mpmenu_parking.py -- park the MeshProperties purpose menu
bug in ROADMAP.txt.

User reported that the purpose enum (default / render / proxy / guide)
needs validation -- the dropdown presents itself, knob value appears to
bind, but the mesh-skip behaviour (for proxy / guide) doesn't visibly
take effect. Needs a proper test pass once the wider doubleSided /
GPU-fix work settles.

Idempotent via marker, CRLF-safe, backup .bak_mpmenu_park.
"""

import argparse
import sys
from pathlib import Path


# Anchor on the 32-material-cap entry (the current last item in the
# "Technical debt / known issues" list). This anchor is stable whether
# or not the earlier HDRI parking patch has landed -- that patch adds
# lines AFTER this anchor, and our OLD still matches.

OLD = """- **32-material cap on noShadowCastMask / shadowCatcherMask**: bitmask
  limits us to 32 unique materials for each flag. Scenes with hero characters
  + dozens of props could brush this limit. Log warning exists; upgrade to
  wider type (uint64_t or a proper per-material flag) when needed.
"""

NEW = """- **32-material cap on noShadowCastMask / shadowCatcherMask**: bitmask
  limits us to 32 unique materials for each flag. Scenes with hero characters
  + dozens of props could brush this limit. Log warning exists; upgrade to
  wider type (uint64_t or a proper per-material flag) when needed.
- **SpectralMeshProperties purpose menu needs validation** (new, 2026-04-20).
  apply_meshprops_fields wired up doubleSided / orientation / purpose, but
  purpose (default / render / proxy / guide) hasn't been tested end-to-end.
  The intended behaviour: when purpose=proxy or purpose=guide, the mesh is
  skipped from the ray-traced render (still shows in the 3D viewer). The
  mesh-skip `continue` is in SpectralRenderIop.cpp around line 3286 guarded
  by `meshPropsHasEntry && (meshPropsPurpose == 2 || meshPropsPurpose == 3)`.
  To validate: (1) confirm the 'mesh props for ...' log shows the purpose
  value flipping when you toggle the enum; (2) confirm 'SpectralRender:
  skipping mesh X (purpose=proxy)' logs when set to proxy; (3) confirm the
  mesh visually disappears from the rendered frame but remains in the
  viewer. Also: decide whether purpose=1 (render) should imply anything
  different from purpose=0 (default). USD treats 'render' as 'always
  include', 'default' as 'parent decides', so currently both render.
"""


EDIT = (
    "Park MeshProperties purpose-menu validation item",
    OLD, NEW,
    "SpectralMeshProperties purpose menu needs validation"
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
    ok = process(args.roadmap, args.dry_run, args.force, ".bak_mpmenu_park")
    if not ok: sys.exit(1)


if __name__ == "__main__":
    main()
