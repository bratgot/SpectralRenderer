#!/usr/bin/env python3
"""
apply_texblend_diag_strip.py -- remove the TexBlendDiag tracers that
were added by apply_texblend_diag.py. Diagnosis is done; textureBlend
is confirmed working end-to-end via apply_gpu_matcheck_full.py.

Files touched: src/SpectralSurfaceOp.cpp, src/SpectralRenderIop.cpp
Idempotent, CRLF-safe, backs up to .bak_texblenddiagstrip.
"""

import argparse
import sys
from pathlib import Path


# Reverse of each TexBlendDiag addition.

SURF_OLD = """    p.textureBlend = _textureBlend;
    fprintf(stderr, "TexBlendDiag: RegisterParams node='%s' textureBlend=%.3f\\n",
        node_name().c_str(), _textureBlend);
"""

SURF_NEW = """    p.textureBlend = _textureBlend;
"""


IOP1_OLD = """                        fprintf(stderr, "TexBlendDiag: looking up shaderPath='%s' in %zu registry entries\\n",
                            shaderPath.c_str(), registry.size());
                        for (const auto& entry : registry) {
                            fprintf(stderr, "TexBlendDiag:   registry key='%s' textureBlend=%.3f\\n",
                                entry.first.c_str(), entry.second.textureBlend);
                            if (shaderPath.find(entry.first) != std::string::npos) {
                                fprintf(stderr, "TexBlendDiag:   HIT '%s' -> mat.textureBlend <- %.3f\\n",
                                    entry.first.c_str(), entry.second.textureBlend);
                                mat.abbeNumber        = entry.second.abbeNumber;
"""

IOP1_NEW = """                        for (const auto& entry : registry) {
                            if (shaderPath.find(entry.first) != std::string::npos) {
                                mat.abbeNumber        = entry.second.abbeNumber;
"""


IOP2_OLD = """                    fprintf(stderr, "TexBlendDiag: AddMaterial mat.textureBlend=%.3f mat.baseColorTexId=%d\\n",
                        mat.textureBlend, mat.baseColorTexId);
                    matId = _scene->AddMaterial(mat);

                    // Remap Project3D projection matrix from temp key to actual matId
"""

IOP2_NEW = """                    matId = _scene->AddMaterial(mat);

                    // Remap Project3D projection matrix from temp key to actual matId
"""


EDITS_SURF = [("Strip RegisterParams tracer", SURF_OLD, SURF_NEW)]
EDITS_IOP = [
    ("Strip registry-lookup tracer",    IOP1_OLD, IOP1_NEW),
    ("Strip AddMaterial tracer",        IOP2_OLD, IOP2_NEW),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n):
    if o not in text and n in text: return text, R.ALREADY
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
    bak = ".bak_texblenddiagstrip"
    ok = True
    ok &= process(args.src / "SpectralSurfaceOp.cpp", EDITS_SURF, args.dry_run, args.force, bak)
    ok &= process(args.src / "SpectralRenderIop.cpp", EDITS_IOP,  args.dry_run, args.force, bak)
    if not ok: sys.exit(1)
    print("\nTracers stripped. Rebuild to clean up stderr output.")


if __name__ == "__main__":
    main()
