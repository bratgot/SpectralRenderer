"""
move_backup_files.py

Moves all *.bak files from anywhere in the repo tree into
<repo>/backupFiles/ to keep the source tree clean.

Recursive: .bak files sit next to the file they back up (so you get
src/SpectralGPU.cpp.bak, CLAUDE.md.bak at root, build.ps1.bak at root,
etc.). We walk the whole tree and gather them all.

Skipped directories: .git, build, PythonFixes, backupFiles. (The first
two are noise; the last two are our tidy destinations that we don't
want to sweep from.)

On name collision in the target, newer wins (overwrites), matching the
convention in move_apply_scripts.py. .bak files are scratch safety
nets, not version history -- if you need history, use git.

Idempotent. Safe to re-run whenever new .bak files accumulate.

Usage:
    python move_backup_files.py                # source = repo root
    python move_backup_files.py <src-dir>      # explicit source
"""

import sys
import shutil
from pathlib import Path

REPO_ROOT = Path(r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral")
TARGET_DIR = REPO_ROOT / "backupFiles"
PATTERN = "*.bak"

SKIP_DIRS = {".git", "build", "PythonFixes", "backupFiles"}


def iter_bak_files(root: Path):
    """Yield all .bak files under root, skipping SKIP_DIRS anywhere in
    the path."""
    for p in root.rglob(PATTERN):
        if not p.is_file():
            continue
        try:
            rel_parts = p.relative_to(root).parts
        except ValueError:
            continue
        # Check only directory parts, not the filename at [-1]
        if any(part in SKIP_DIRS for part in rel_parts[:-1]):
            continue
        yield p


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO_ROOT
    if not src.is_dir():
        print(f"ERROR: source is not a directory: {src}")
        return 1

    if src.resolve() == TARGET_DIR.resolve():
        print(f"ERROR: source and target are the same: {src}")
        print("       Nothing to do -- already organized.")
        return 2

    TARGET_DIR.mkdir(parents=True, exist_ok=True)

    files = sorted(iter_bak_files(src))
    if not files:
        print(f"No {PATTERN} files found under {src}")
        print(f"Target dir exists: {TARGET_DIR}")
        return 0

    moved = 0
    overwritten = 0
    for f in files:
        dest = TARGET_DIR / f.name
        if dest.exists():
            overwritten += 1
            print(f"  Overwriting: {dest.name}")
        shutil.move(str(f), str(dest))
        rel = f.relative_to(src)
        print(f"  Moved: {rel}  ->  backupFiles/")
        moved += 1

    print()
    print(f"Done. {moved} file(s) moved to {TARGET_DIR}")
    if overwritten:
        print(f"      ({overwritten} existing file(s) overwritten)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
