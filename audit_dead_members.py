"""
audit_dead_members.py

Scans all .h files under a source directory and reports class member
declarations that have zero references anywhere else in the tree.

Heuristic: matches lines that look like member declarations starting
with our naming convention (underscore-prefix, e.g. `_foo`) in a class
context, then greps for references to each name across all .cpp, .cu,
and .h files (excluding the declaration line itself).

Members with 1 total occurrence (the declaration) are flagged as
probably-dead. Members with >1 are considered live.

This is a heuristic, not a proof. False positives happen:
    - Members referenced only in macros the regex doesn't understand
    - Members written by CUDA kernels that parse the struct differently
    - Members accessed through reflective offsetof / memcpy patterns
Always verify each reported member by looking at it before removing.

False negatives also happen for the same reasons in reverse.

Usage:
    python audit_dead_members.py                    # scans src/
    python audit_dead_members.py <src-dir>
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

DEFAULT_SRC = Path(
    r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\src"
)

SCAN_EXTENSIONS = {".h", ".hpp"}
SEARCH_EXTENSIONS = {".cpp", ".cu", ".h", ".hpp", ".cuh"}

# Matches a member declaration line. Captures the member name.
# Examples that match:
#     CUstream               _stream     = nullptr;
#     unsigned int           _allocW = 0;
#     std::vector<CUdeviceptr> _d_texPixels;
#     DeviceVolume           _d_volumes[SPECTRAL_MAX_GPU_VOLUMES];
#     float                  _envSH[4][3] = {};
# Examples that do NOT match (intentionally):
#     method declarations:   void _FreeAccel();
#     static methods:        static bool _Helper(...);
#
# Strategy: require the name followed by `=`, `;`, `[`, or `{` (not `(`).
MEMBER_PATTERN = re.compile(
    r"""
    ^\s+                           # indentation
    (?:static\s+)?                 # optional static
    (?:const\s+)?                  # optional const
    (?:mutable\s+)?                # optional mutable
    [A-Za-z_][\w:<>,\s\*&]*?       # type (greedy, allows templates/refs/ptrs)
    \s+                            # space before name
    (_[A-Za-z_]\w*)                # member name starting with underscore
    \s*                            # optional space
    (?:=|;|\[|\{)                  # followed by init/end/array -- NOT `(`
    """,
    re.VERBOSE,
)


def find_members_in_file(path: Path):
    """Yield (name, linenum) for each member declaration in path."""
    try:
        content = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return
    for lineno, line in enumerate(content.splitlines(), start=1):
        m = MEMBER_PATTERN.match(line)
        if m:
            yield m.group(1), lineno


def count_references(name: str, src_dir: Path) -> int:
    """Count total occurrences of `name` across all search-extension files.
    Uses a word-boundary regex to avoid false positives from partial
    name overlaps (e.g. _stream vs _streamAlt)."""
    pattern = re.compile(r"\b" + re.escape(name) + r"\b")
    count = 0
    for ext in SEARCH_EXTENSIONS:
        for p in src_dir.rglob(f"*{ext}"):
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            count += len(pattern.findall(text))
    return count


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    if not src.is_dir():
        print(f"ERROR: not a directory: {src}")
        return 1

    # Gather all declared members: name -> list of (filepath, lineno)
    declarations = defaultdict(list)
    for ext in SCAN_EXTENSIONS:
        for h in src.rglob(f"*{ext}"):
            for name, lineno in find_members_in_file(h):
                declarations[name].append((h, lineno))

    if not declarations:
        print(f"No member declarations found under {src}")
        return 0

    print(f"Scanned {len(declarations)} unique member names "
          f"across {src}\n")

    dead = []
    for name in sorted(declarations.keys()):
        refs = count_references(name, src)
        decl_count = len(declarations[name])
        # A member is "dead" if total references == declarations
        # (meaning it's only mentioned where it's declared).
        if refs <= decl_count:
            dead.append((name, declarations[name], refs))

    if not dead:
        print("No suspected-dead members found. Codebase is clean.")
        return 0

    print(f"Found {len(dead)} suspected-dead member(s):\n")
    for name, decls, refs in dead:
        print(f"  {name}  (refs={refs}, declarations={len(decls)})")
        for (path, lineno) in decls:
            rel = path.relative_to(src) if src in path.parents else path
            print(f"      at {rel}:{lineno}")
    print()
    print("These are HEURISTIC matches. Verify each one before removing:")
    print("  - Check for macro references the regex missed")
    print("  - Check for CUDA kernel access patterns")
    print("  - Check for friend class access")
    return 0


if __name__ == "__main__":
    sys.exit(main())
