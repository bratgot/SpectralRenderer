#!/usr/bin/env python3
"""
apply_roadmap_hdrivis_parking.py -- park the two live HDRI-visible
bugs in ROADMAP.txt's "Technical debt / known issues" section.

Two entries added:
  1. HDRI visible checkbox doesn't affect GPU/CPU background render
  2. Nuke crashed when user interacted with the file path knob on
     SpectralEnvLight

Idempotent via marker, CRLF-safe, backup to .bak_hdrivis_park.
"""

import argparse
import sys
from pathlib import Path


OLD = """- **32-material cap on noShadowCastMask / shadowCatcherMask**: bitmask
  limits us to 32 unique materials for each flag. Scenes with hero characters
  + dozens of props could brush this limit. Log warning exists; upgrade to
  wider type (uint64_t or a proper per-material flag) when needed.

---
"""

NEW = """- **32-material cap on noShadowCastMask / shadowCatcherMask**: bitmask
  limits us to 32 unique materials for each flag. Scenes with hero characters
  + dozens of props could brush this limit. Log warning exists; upgrade to
  wider type (uint64_t or a proper per-material flag) when needed.
- **HDRI "visible in background" checkbox does not render the BG** (new,
  2026-04-20). apply_hdri_visible patch added the knob on SpectralEnvLight
  plus the full plumbing: host member, SpectralLight.visibleInPrimary field,
  stamp at both hdriDome sites, CPU primary-miss gate, GPU LaunchParams
  fields, GPU miss-shader HDRI sampling. Compiles and runs, but toggling
  the checkbox does not change what the user sees. Suspects in priority
  order: (1) checksum miss -- _envVisibleBg change may not invalidate
  the GPU params upload (verify with a log line in the lightCheck block);
  (2) the extract loop populates from the first Dome that HAS visibleInPrimary
  set, so if the user's scene has multiple dome lights and the flag is on
  the wrong one, code takes the first hit; (3) CPU path: verify the flag
  is making it through SpectralLight copy construction (the light is
  copied into `scene._lights` by value); (4) the envTexId the kernel
  samples may be pointing at the wrong texture (HDRI tex upload order
  could have shifted since the field was added). Triage plan: add an
  SLOG at each stamp site (Iop, host extract, LaunchParams fill) printing
  envVisibleBg + envTexId, then toggle the knob and watch the log
  transitions.
- **Nuke crash when clicking the HDRI file-path knob on SpectralEnvLight**
  (new, 2026-04-20). User reported a crash after interacting with the
  file browser on SpectralEnvLight -> HDRI -> file. No trace captured yet.
  First suspects: the File_knob browse callback pathing (unicode? network
  share?), the existing cold-load-to-texId path (SpectralRenderIop.cpp
  ~line 8452 `el->hdriFile && strlen(el->hdriFile) > 0`), or a LoadTexture
  reentrancy during active render. Next time it repros: run Nuke from a
  cmd prompt and look for stderr, or attach Visual Studio debugger. A
  defensive try/catch around the file-path reads + LoadTexture call would
  reduce blast radius if the exception path is the cause.

---
"""


EDIT = (
    "Park HDRI-visible live bugs in known-issues section",
    OLD, NEW,
    "HDRI \"visible in background\" checkbox does not render the BG"
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
    ok = process(args.roadmap, args.dry_run, args.force, ".bak_hdrivis_park")
    if not ok: sys.exit(1)


if __name__ == "__main__":
    main()
