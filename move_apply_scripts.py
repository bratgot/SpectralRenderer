"""
move_apply_scripts.py

Organizes all apply_*.py scripts from a source directory into
<repo>/PythonFixes/ so they stop cluttering the repo root.

Default source: the repo root. Default target: <repo>/PythonFixes/.
Creates the target if missing. Existing files in the target with the
same name are overwritten (newer script wins -- useful because
apply scripts get revised during a session).

Can be re-run safely as new apply scripts get downloaded into the
repo root -- each run picks up whatever's new and leaves an already-
tidy target untouched.

Usage:
    python move_apply_scripts.py                # source = repo root
    python move_apply_scripts.py <src-dir>      # explicit source

To also keep PythonFixes/ out of git, add this line to .gitignore:
    PythonFixes/
Decide separately -- if you want the apply history versioned as an
audit trail, leave it tracked.
"""

import sys
import shutil
from pathlib import Path

REPO_ROOT = Path(r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral")
TARGET_DIR = REPO_ROOT / "PythonFixes"
PATTERN = "apply_*.py"


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
    if not TARGET_DIR.exists():
        # mkdir should have created it; if this fires, permissions problem.
        print(f"ERROR: could not create {TARGET_DIR}")
        return 3

    # Non-recursive glob in src only.
    scripts = sorted(p for p in src.glob(PATTERN) if p.is_file())

    if not scripts:
        print(f"No {PATTERN} files found in {src}.")
        print(f"Target dir exists: {TARGET_DIR}")
        return 0

    moved = 0
    overwritten = 0
    for script in scripts:
        dest = TARGET_DIR / script.name
        if dest.exists():
            overwritten += 1
            print(f"  Overwriting: {dest.name}")
        shutil.move(str(script), str(dest))
        print(f"  Moved: {script.name}  ->  PythonFixes/")
        moved += 1

    print()
    print(f"Done. {moved} script(s) moved to {TARGET_DIR}")
    if overwritten:
        print(f"      ({overwritten} existing file(s) overwritten with newer versions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
