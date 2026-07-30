"""
write_session_handoff.py

Writes SESSION_HANDOFF.md in the repo root with a fresh brief for the
2026-04-23 session, meant to hand off to tomorrow-you (or a new Claude
chat if the conversation window rolls over).

Behavior:
    - If SESSION_HANDOFF.md doesn't exist: creates it.
    - If it exists with identical content: noop ("already applied").
    - If it exists with different content: backs up to .bak and
      overwrites with the new content.

Idempotent, same convention as the other apply scripts.

Usage:
    python write_session_handoff.py              # target = repo root
    python write_session_handoff.py <path>       # explicit path
"""

import sys
import shutil
from pathlib import Path

DEFAULT_PATH = Path(
    r"C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\SESSION_HANDOFF.md"
)

CONTENT = """# SpectralRenderer -- Session Handoff 2026-04-23 -> next

Paste this into a new Claude chat along with CLAUDE.md. The next Claude
should read CLAUDE.md first, then this brief, and will be caught up.

## What I'm working on

SpectralRenderer: spectral path-tracing plugin for Nuke 17.
Windows 11, VS2022, CUDA 12.6, OptiX 9.0, RTX 5060 Ti.
Repo: `C:\\dev\\SpectralRenderer\\HdSpectral_Phase1\\HdSpectral`
Branch: `main`. Remote: github.com/bratgot/SpectralRenderer
Build: `.\\build.ps1` (wraps CMake/MSBuild; see "Today's accomplishments"
for today's hardening)

I've built C++/Python/Blinkscript Nuke tools before.

## Ops architecture (quick ref)

Four Ops write to static registries read by SpectralRenderIop:
- **SpectralSurfaceOp** (ShaderOp) -- material knobs -> SpectralParams
- **SpectralShadowCatcherOp** (ShaderOp) -- shadow-catcher surface
- **SpectralDraftingOp** (ShaderOp, renamed from SpectralWireframeOp) --
  line-art material
- **SpectralMeshPropertiesOp** (GeomOp) -- per-mesh overrides (subdiv,
  normals, visibility, etc.)

All four have:
- Destructor erasing by `node_name()` (Mitigation A for registry leaks
  on delete/close)
- `_lastRegisteredName` member + rename-detection in `RegisterParams`
  (Mitigation B for in-place rename)
- `node_disabled()` handling in `RegisterParams` (erase on disable)

Other key ops:
- **SpectralRenderIop** -- main Iop, scene read + render dispatch.
  10,000+ lines. `append(hash)` uses an explicit knob list (deliberately
  NOT `Iop::append(hash)` -- see CLAUDE.md).
- **SpectralEnvLight** (SourceGeomOp) -- HDRI + sky, pipe-only HDRI
  input (input 1).
- **SpectralVDBRead**, **SpectralVolMerge**, **SpectralVolumeMaterial**,
  **SpectralStudioLight** -- volumetric + lighting.

## Session style I prefer

- **Mechanical apply scripts** for source edits: Python, anchored
  `str_replace`, `.bak` backups, idempotent via `new in text` check.
  Pattern well-established by now. For files where line endings matter
  (build.ps1, the .h files), use the binary-read / normalize-to-LF /
  re-emit-with-original-endings variant to preserve CRLF vs LF exactly.
- **Don't roll up `.patch` files** -- share edited source directly or
  use an apply script. (Memory preference.)
- **Commit messages via `git commit -F file.txt` with `-Encoding ASCII`**
  in PowerShell to avoid BOM contamination. Or multi-`-m` on the
  command line -- also ASCII.
- **I close Nuke, rebuild, reopen** for iteration. build.ps1 as of
  today warns yellow when a real build's DLL mtime doesn't advance
  (the LNK1104 / lock signature).
- **Tests** are me running Nuke and eyeballing, not automated. Don't
  over-prescribe test rituals.
- **Trust-but-verify**. If I say "all works", I mean it, but I'm happy
  to be asked "which exact behaviours did you check" when it matters.

## Today's (2026-04-23) accomplishments

**Rendering / correctness:**
- **Per-Iop CUDA stream** replaces device-wide `cudaDeviceSynchronize`.
  Every SpectralGPU instance creates its own `cudaStream_t` in
  `Initialize` and destroys in `Cleanup`. All 5 sync sites switched
  from `cudaDeviceSynchronize()` to `cudaStreamSynchronize(_stream)`.
  OptiX launches and the NanoVDB densify kernel now run on `_stream`
  instead of the null stream. Multiple SpectralRenderIop instances
  can coexist without stomping each other. Verified in logs -- each
  instance prints its own stream pointer on init.
- **`SPECTRAL_FAST_COMPILE=1` now uses LEVEL_1** instead of LEVEL_0.
  After the /pdfLight MIS fix, raygen grew past OptiX's -O0 compile
  ceiling (error 7299). LEVEL_1 enables just enough DCE / block-
  merging to fit while still being cheap.

**Code quality (SpectralGPU.h):**
- Removed dead `_uploadStream`, `_pinnedStaging`, `_pinnedSize`
  (declared, never referenced -- aspirational-only async upload
  scaffolding).
- Removed dead `_d_volumeDensity`, `_d_volumeTemp`, `_volCachedSize`
  ("Legacy single-volume (kept for cleanup)" -- comment was
  misleading, nothing was allocated so there was nothing to clean up).

**Infrastructure / build:**
- **build.ps1 hardening pass:**
  - Error regex now matches `LNK\\d+` (catches LNK1104 output-locked
    failures that previously slipped past).
  - After BUILD SUCCEEDED, reports DLL mtime with three branches:
    fresh (<30s) -> green, incremental no-op -> green, stale +
    build did work -> YELLOW. The last is the actual
    lock-induced-silent-fail signal; the only noisy case is the
    one that matters.
- **launch_nuke.bat double-launch bug fixed** -- old version
  unconditionally launched Nuke, then hit an `if --log` block
  that relaunched it (Nuke would "come back" after closing).
- **.gitattributes added**: text normalizes to LF on checkin;
  Windows-native files (.sln, .vcxproj*, .bat, .ps1, .cmd) keep
  CRLF; binary formats explicit. Requires a one-time renormalize
  commit to take effect on the existing mixed-endings tree.
- **.gitignore** now covers `*.bak`.

**Organization:**
- `PythonFixes/` holds apply_*.py scripts (via `move_apply_scripts.py`).
- `backupFiles/` holds *.bak files (via `move_backup_files.py`).
- Both move scripts are idempotent; re-run as new scripts or .bak
  files accumulate.

**Docs (CLAUDE.md):**
- DLL-timestamp section corrected. Previous step 3 told readers to
  compare the `SpectralRender: DLL build ...` log line with the
  filesystem mtime and expect them to match -- this turns out to be
  the wrong direction of unreliability. The log string is a cached
  literal that can lag by a build or more. Filesystem mtime is ground
  truth.
- Same section generalized to cover ALL plugins that print a build
  timestamp (SpectralVDBRead has the same idiom). Step 2's PowerShell
  command now lists DLLs tree-wide rather than a single file.
- Backlog: retired the CUDA-stream and FAST_COMPILE bullets
  (both landed today). FAST_COMPILE dev-iteration section body
  updated from LEVEL_0 to LEVEL_1 with breadcrumb explaining why.

## Active backlog (4 items)

1. **`Iop::append(hash)` refactor** -- deferred with documented
   reasoning in CLAUDE.md. Would sweep viewport-preview knobs into
   the render cache key. Don't revisit unless a "toggle X does
   nothing" bug resurfaces.

2. **HDRI intensity calibration** post-MIS-fix. Watch-task: the
   /pdfLight bug fix (2026-04-22) made HDRI renders substantially
   dimmer. After a few days at the new baseline, decide whether to
   bump default `hdri_intensity` (~2-3x suspected) so neutral
   scenes match expectations. Until then, users opening old
   scripts may need to manually push intensity up.

3. **HDRI Read-pipe empty-validate detection** -- watch-note only,
   couldn't reproduce. Full plan in CLAUDE.md if it resurfaces.

4. **HDRI sphere preview in 3D viewport** for SpectralEnvLight.
   The node currently draws a wireframe hemisphere + compass rose
   + sun arrow (sky model), but no visual representation of the
   pipe-connected HDRI. Plan: textured hemisphere with a pre-
   tonemapped thumbnail, rotating with `hdriRotate`. Requires
   pipe-read in SpectralEnvLight::_validate (currently only
   SpectralRenderIop walks the input chain), downsample +
   Reinhard tonemap at upload, GL texture lifecycle tied to Op
   lifetime. Scoped ~200-300 LoC, compile-test iteration. Full
   plan in CLAUDE.md.

## Candidate places to start next session

- **Feature**: HDRI sphere preview (backlog item 4) -- most scoped
  real work pending, natural next beat. Worth a design convo at
  the start.
- **Small housekeeping**: consistency sweep for SpectralWireframe
  -> SpectralDrafting rename. CLAUDE.md still references the old
  name in a few places (Hash invalidation example at line ~35,
  file layout section). Noted but not done today.
- **Speculative**: audit other .cpp files for dead members
  (SpectralIntegrator, SpectralBVH, etc.) -- today we found two
  such blocks in SpectralGPU.h; might be more elsewhere.
- **Watch-tasks**: HDRI intensity calibration (item 2), HDRI
  pipe-empty detection (item 3). No action unless signal arrives.
- **Something larger**: volumetric refraction, SpectralSurface PBR
  expansion (Phase 21), prebuilt material library (Phase 18). Less
  scoped in CLAUDE.md.

## Known state issues / gotchas fresh Claude should internalise

- **Codebase had mixed line endings pre-today** (some .cpp as LF,
  some .h as CRLF). `.gitattributes` was added today but the
  `git add --renormalize . && git commit` normalization commit
  may or may not have landed. Check with `git status` -- a pending
  big "everything changed" diff is the renormalize commit waiting
  to happen.
- **Device-mode switching (CPU <-> GPU)** forces full rebuild,
  masking per-knob invalidation signals. When diagnosing "GPU
  doesn't update", stay in GPU mode the whole test.
- **Short copyright paste-backs** from Claude are encouraged; long
  source blocks aren't. Prefer apply scripts.
- **DLL build-timestamp log lines are not reliable** -- they lag
  the actual binary. Trust filesystem mtime. Applies to every
  plugin with this pattern: `SpectralRender: DLL build ...`,
  `SpectralVDBRead: build ...`, future siblings.
- **Project file snapshots in `/mnt/project/`** (if you're in a
  fresh Claude chat) are stale vs. what's on disk. Upload current
  source explicitly for real edits.

## Commit log from today (for verification)

```
git log --oneline --since="2026-04-23 00:00"
```

Should show roughly a dozen commits, prefixed `spectral:`, `build:`,
`docs:`, `repo:`, or `chore:`.

## Files to pass to next Claude

1. This brief
2. The current `CLAUDE.md`
3. Whatever specific source files become relevant to the next task
   (don't pre-emptively dump the whole `src/`)
"""


def main() -> int:
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PATH

    # Normalize both sides to LF for comparison so a CRLF/LF mismatch
    # doesn't falsely report "different content".
    new_content_lf = CONTENT.replace("\r\n", "\n")

    if target.exists():
        existing = target.read_text(encoding="utf-8")
        existing_lf = existing.replace("\r\n", "\n")
        if existing_lf == new_content_lf:
            print(f"{target} already matches. Nothing to do.")
            return 0
        # Different -- back up existing, then write new.
        bak = target.with_suffix(target.suffix + ".bak")
        shutil.copy2(target, bak)
        print(f"Backup: {bak}")

    # Write with LF endings. git + Markdown tooling doesn't care.
    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write(CONTENT)
    print(f"Wrote: {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
