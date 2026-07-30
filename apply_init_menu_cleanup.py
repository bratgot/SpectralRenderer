"""
apply_init_menu_cleanup.py

Three coordinated cleanups:

    1. init.py  -- update force-load comment to list ALL current Op classes,
                   not the stale 4-of-9 list from when it was written.
    2. init.py  -- replace em-dashes with ASCII per CLAUDE.md style guide.
    3. menu.py  -- replace em-dashes and box-drawing chars with ASCII;
                   add the missing icon=SpectralEnvLight.png to its menu
                   entry (the icon file exists, the menu.py just forgot
                   to wire it).

Both files are LF in source -- script preserves whatever it finds.

Usage:
    python apply_init_menu_cleanup.py [path-to-repo-root]
"""

import sys
import shutil
from pathlib import Path

DEFAULT_ROOT = Path(
    r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral"
)

# -------------------------------------------------------------------------
# init.py edits
# -------------------------------------------------------------------------

INIT_EDITS = [
    # 1. Em-dash in the file header
    (
        "# SpectralRenderer \u2014 plugin loader\n",
        "# SpectralRenderer -- plugin loader\n",
        "init.py: header em-dash",
    ),
    # 2. Em-dash in the version-skip comment
    (
        "    pass  # silently skip \u2014 plugin won't load in older Nuke\n",
        "    pass  # silently skip -- plugin won't load in older Nuke\n",
        "init.py: skip-comment em-dash",
    ),
    # 3. Update the stale force-load comment list. The original lists
    #    4 Ops; today's actual register list (per menu.py) is 10. Also
    #    swaps the em-dash on the third line of the comment block.
    (
        "    # Force-load SpectralRender.dll so ALL node classes register at startup\n"
        "    # (SpectralRender, SpectralSurface, SpectralVDBRead, SpectralVolumeMaterial)\n"
        "    # Without this, only SpectralRender registers on first use \u2014 the others\n"
        "    # show \"Unknown command\" until a SpectralRender node is created.\n",
        "    # Force-load SpectralRender.dll so ALL node classes register at startup.\n"
        "    # Currently registered: SpectralRender, SpectralSurface, SpectralDrafting,\n"
        "    # SpectralShadowCatcher, SpectralVolumeMaterial, SpectralMeshProperties,\n"
        "    # SpectralVDBRead, SpectralVolMerge, SpectralEnvLight, SpectralStudioLight.\n"
        "    # Without this, only SpectralRender registers on first use -- the others\n"
        "    # show \"Unknown command\" until a SpectralRender node is created.\n",
        "init.py: refresh force-load comment + em-dash",
    ),
]

# -------------------------------------------------------------------------
# menu.py edits
# -------------------------------------------------------------------------

MENU_EDITS = [
    # 1. Header em-dash
    (
        "# SpectralRenderer \u2014 menu.py\n",
        "# SpectralRenderer -- menu.py\n",
        "menu.py: header em-dash",
    ),
    # 2-5. Box-drawing-char section dividers -> ASCII
    (
        "    # \u2500\u2500\u2500 Rendering \u2500\u2500\u2500\n",
        "    # --- Rendering ---\n",
        "menu.py: Rendering divider",
    ),
    (
        "    # \u2500\u2500\u2500 Materials \u2500\u2500\u2500\n",
        "    # --- Materials ---\n",
        "menu.py: Materials divider",
    ),
    (
        "    # \u2500\u2500\u2500 Scene \u2500\u2500\u2500\n",
        "    # --- Scene ---\n",
        "menu.py: Scene divider",
    ),
    (
        "    # \u2500\u2500\u2500 Lighting \u2500\u2500\u2500\n",
        "    # --- Lighting ---\n",
        "menu.py: Lighting divider",
    ),
    # 6. Add the missing icon arg to SpectralEnvLight entry. Icon file
    #    SpectralEnvLight.png exists in icons/current/.
    (
        '    m.addCommand("Lighting/SpectralEnvLight", "nuke.createNode(\'SpectralEnvLight\')")\n',
        '    m.addCommand("Lighting/SpectralEnvLight", "nuke.createNode(\'SpectralEnvLight\')", icon="SpectralEnvLight.png")\n',
        "menu.py: add SpectralEnvLight icon",
    ),
]

FILE_EDITS = {
    "init.py": INIT_EDITS,
    "menu.py": MENU_EDITS,
}


def apply_to_file(path: Path, edits) -> int:
    if not path.is_file():
        print(f"  ERROR: not a file: {path}")
        return 1

    with open(path, "rb") as f:
        raw = f.read()

    has_crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")

    new_text = text
    applied = 0
    already = 0
    for old, new, label in edits:
        old_n = new_text.count(old)
        if old_n == 0:
            if new_text.count(new) >= 1:
                print(f"    [{label}] already applied")
                already += 1
                continue
            print(f"    [{label}] ERROR: anchor not found")
            return 2
        if old_n > 1:
            print(f"    [{label}] ERROR: matches {old_n} times -- ambiguous")
            return 3
        new_text = new_text.replace(old, new, 1)
        applied += 1
        print(f"    [{label}] applied")

    if applied == 0:
        print(f"  All {already} edits already applied in {path.name}.")
        return 0

    bak = path.with_suffix(path.suffix + ".bak")
    shutil.copy2(path, bak)
    print(f"  Backup: {bak}")
    out = new_text.replace("\n", "\r\n") if has_crlf else new_text
    with open(path, "wb") as f:
        f.write(out.encode("utf-8"))
    print(f"  Wrote: {path}")
    return 0


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ROOT
    if not root.is_dir():
        print(f"ERROR: not a directory: {root}")
        return 1

    for filename, edits in FILE_EDITS.items():
        print(f"== {filename} ==")
        rc = apply_to_file(root / filename, edits)
        if rc != 0:
            return rc
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
