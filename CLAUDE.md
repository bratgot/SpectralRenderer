# SpectralRenderer - Notes for Claude

Project: spectral path-tracing plugin for Nuke 17. VS2022, CUDA 12.6, OptiX 9.0,
RTX 5060 Ti, Windows 11. Build via `.\build.ps1` which wraps CMake/MSBuild.

Before writing or modifying code, read the relevant sections below. Many of the
non-obvious gotchas have cost real debugging cycles.

---

## Nuke plugin architecture (NDK + GeoScene)

### Hash invalidation -- the single biggest gotcha

Nuke caches each Op's output keyed by its hash. **If a knob is not hashed, the
cache is never invalidated when that knob changes.** Symptoms: "I toggle the
checkbox but nothing happens until I scrub the timeline."

- `Iop::append(Hash&)` controls what contributes to the cache key.
- When overriding `append`, include either:
  - `Iop::append(hash);` at the top (hashes all knobs automatically, canonical
    idiom, preferred default)
  - Or every knob that affects output, explicitly, by name.
- Returning `1` from `knob_changed` means "I handled it" -- it does **not**
  invalidate the hash. Hash invalidation only happens via `append` or via
  explicit `asapUpdate()` / input hash propagation.
- Any knob that changes the rendered output **must** be in the hash. Examples
  that have bitten us: `_scanlineCompat`, `_useBuiltinLight`, `_deviceMode`.
- Registries of external nodes (e.g. `SpectralWireframeOp::GetRegistry()`) must
  also be hashed in the consuming Iop's `append`, otherwise toggling a
  SpectralWireframe knob won't invalidate SpectralRender.

When adding any new knob, the mental checklist is:
1. Does this knob affect the rendered frame output? If yes -> must be hashed.
2. Does it affect viewport preview only? Register it via `knob_changed` and
   mark viewport dirty, but no hash needed.
3. Does it affect another node's behaviour? That node's hash needs the knob too.

### Node disable behaviour

When a user disables a node in Nuke (`D` key, or `node_disabled()` returns true):
- The Op still exists and still gets `_validate` called.
- Downstream behaviour depends on the Op type -- for most geometry modifiers,
  being disabled means "pass through unchanged."
- **Any side effects the Op has on external state must be reversible.** If the
  Op writes into a static registry (see below), it must **remove** that entry
  when disabled. Otherwise the override persists even when the node is disabled.
- The consuming Iop (usually SpectralRenderIop) must hash the registry size and
  contents so that a disable-driven registry change actually forces re-render.

### Static registries for cross-node communication

Pattern used by SpectralSurfaceOp, SpectralWireframeOp, SpectralShadowCatcherOp,
SpectralMeshPropertiesOp: write knob values into a `static std::unordered_map`
keyed by `node_name()`, read from it in SpectralRenderIop at render time.

Rules:
- Population: call `RegisterParams()` from both `knob_changed` (for UI tweaks)
  and `_validate` (for cold renders where the user hasn't touched the node).
- Respect `node_disabled()` in `RegisterParams` -- if disabled, `erase` instead
  of `insert`.
- Node rename/delete does not clean up entries -- add a destructor if needed.
- The matching code in SpectralRenderIop must use a meaningful key. Matching by
  substring of `node_name()` against the USD prim path is brittle: ShaderOps
  end up in the shader prim path, but **GeomOps do not end up in any USD path**.
  Use `size() == 1` as a fallback only, and flag it as a known limitation.

### GeomOp vs ShaderOp vs SourceGeomOp

- `ShaderOp`: emits a shader into the USD material graph. Its `node_name()`
  ends up in the UsdShader prim path, so `shaderPath.find(node_name())` works
  for matching.
- `GeomOp`: inline modifier in the geometry chain. Does **not** appear in any
  USD prim path. Must use a different mechanism (registry keyed by mesh prim
  path, or walk the Op input chain in the consumer) to associate settings with
  specific geometry.
- `SourceGeomOp`: creates prims into the scene context. Use this for lights,
  volumes, and anything that adds scene content rather than modifying it.

Engine constructors take `GeomOpNode* parent`. Do **not** use `firstOp()` from
inside the Engine's `processScenegraph` to reach back to the owning Op --
behaviour is not guaranteed. Instead, do the registration in the Op's
`_validate` and make `processScenegraph` a pass-through.

### Knob flags that matter

- `KnobModifiesAttribValues(f)` on a knob hints to Nuke that the knob value
  should propagate into USD attributes. It does **not** by itself write the
  attribute -- you still have to do that in `processScenegraph`.
- `Knob::STARTLINE` / `ClearFlags(f, Knob::STARTLINE)` controls UI layout, not
  data flow.
- `SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION)` for read-only display knobs.

### Knob modifier ordering

`SetRange`, `SetFlags`, `ClearFlags`, and `Tooltip` apply to the **most
recently created knob**, not to any named knob. Insert a new knob between
an existing `_knob(...)` declaration and its modifiers, and all the
modifiers silently retarget:

```cpp
Double_knob(f, &foo, "foo", "foo");
Bool_knob(f, &bar, "bar", "bar");   // <-- inserted later
SetRange(f, 0, 360);                // now applies to `bar`, not `foo`!
Tooltip(f, "foo tooltip");          // now appears on `bar` hover
```

Consequence: `foo` silently defaults to Nuke's 0..1 slider range, `bar`
gets a meaningless range (ignored for Bool) and the wrong tooltip. Has
bitten us on HDRI rotate + visible-in-BG -- slider read 0..1 after the
visibility checkbox was added between rotate's declaration and its
SetRange, and nobody noticed until a user tried to rotate.

Rule: **put each knob's modifiers immediately after its `_knob(...)`
declaration**, before the next knob. When inserting a new knob between
existing ones, move the old knob's modifiers up first:

```cpp
Double_knob(f, &foo, "foo", "foo");
SetRange(f, 0, 360);                // always directly after
Tooltip(f, "foo tooltip");
Bool_knob(f, &bar, "bar", "bar");
ClearFlags(f, Knob::STARTLINE);     // bar's modifiers
Tooltip(f, "bar tooltip");
```

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

### DLL build timestamp can be stale (Windows file-lock quirk)

`SpectralRender.dll` has a compiled-in build timestamp that gets logged on
load: `SpectralRender: DLL build <date> <time>`. It's useful for confirming a
rebuild actually reached the running process -- but **don't trust "same
timestamp" as proof of "same binary"** in every case.

Failure mode: Nuke is open, `.\build.ps1` runs, reports BUILD SUCCEEDED, but
the DLL on disk doesn't change because Windows blocks the overwrite while
Nuke has the DLL mmap'd. Build tools may report success because the linker
step appears to have run, without failing loudly on the locked output file.
Next Nuke session loads the OLD DLL. Developer sees no effect from their
patch, log timestamp matches the previous session, and concludes "my diag
didn't fire / my fix didn't work" when in reality the new binary never got
loaded.

Diagnostic steps when "my patch didn't work":
1. Check Task Manager: is `Nuke17.exe` or `Nuke15.exe` (etc.) actually gone?
2. Check the DLL's filesystem timestamp (`Get-Item
   build\SpectralRender\SpectralRender.dll | Select LastWriteTime`). Compare
   to when you ran build.ps1.
3. Compare the `SpectralRender: DLL build ...` log line with that filesystem
   timestamp. They should match.

Mitigation: always fully close Nuke (not just the file) before rebuilding.
Rebuild may need to be run from a fresh PowerShell if VS/MSBuild caches
something about the previous output state.

### MSVC lambda capture quirk

MSVC requires `constexpr` locals to be explicitly captured in lambdas. GCC/Clang
let this slide. Either:
- Use `[&]` / `[=]` default capture, or
- Inline the constant value into the lambda body directly.

The second is preferred for small constants -- no capture overhead, no ambiguity.

### CUDA: device-function lambdas can silently return stale memory

On CUDA 12.6 / OptiX 9.0, `auto f = [&](...) {...}` inside a
`__device__` function can silently return uninitialised / stale memory
when the lambda captures locals by reference. No compile warning, no
sanitizer fire, no CUDA runtime error -- the kernel just produces
wrong output, reproducibly.

Root-caused via an HDRI surface-shading bug ("flat grey cube under
HDRI"): `sampleEnvHDRI` and `sampleTextureGPU` both used
`auto px = [&](int x, int y) { ... };` to do their bilinear taps. The
lambda loaded correct values on the first tap and then drifted to
stale/garbage on subsequent taps from the same call. Inlining the
lambda body into four explicit reads (one per tap) fixed it fully.

Rule for device code: **do not use lambdas inside `__device__` /
`__forceinline__ __device__` functions**, even trivial ones, even with
`[&]`. Inline the body. If you need shared logic, factor to a
`__forceinline__ __device__` helper function, not a lambda.

This is a device-side issue; host-side C++ lambdas are fine. Cost us
most of a debugging day; the symptom looks like a physics / shading
bug, not a memory bug, so the right hypothesis comes last.

**Collateral hazard: inline-expansion drops context.** When de-lambda'ing
by inlining the body at the callsite (correct fix for the stale-memory
bug above), it's easy to drop surrounding logic that lived in the outer
function. Hit this on 2026-04-22: the "visible in BG" path inlined a
copy of `sampleEnvHDRI`'s body at the miss shader, but the inlined copy
dropped the HDRI rotation block. Result: rotating the HDRI rotated the
sphere lighting but left the BG plate stationary.

Rule: when inlining a device function as a de-lambda fix, treat the
inline site as a **full replacement** for the original call, including
every preprocessing / post-processing block. Diff the inlined body
against the original function and confirm every non-lambda line is
represented. It is not enough to just inline the lambda body -- the
surrounding code in the original function is also the contract.

### UTF-8 in string literals

MSVC produces C2022 errors on non-ASCII bytes in `char*` string literals. Do
not paste em-dashes, en-dashes, Unicode arrows, box-drawing characters, or
multiplication signs into C strings. Use ASCII substitutes:
- `—` em-dash -> `--` or `-`
- `→` arrow  -> `->`
- `×` times  -> `x` or `*`

Python cleanup that uses `\xHH` escape sequences produces literal `\xHH` text
in the file if done wrong -- strip those too.

### `M_PI` and `_USE_MATH_DEFINES`

On MSVC, `M_PI` is not defined unless `_USE_MATH_DEFINES` is set before
including `<cmath>`. Simpler to use a literal `3.14159265f`.

### Float positions and hash keys

Using `std::hash<float>` with exact `==` comparison for vertex welding fails
on meshes with "separate vertices" topology (Nuke GeoSphere, Nuke GeoSphere
with unshared verts, etc.): corresponding positions can differ by a few ULPs
after transform multiplication, and the hash treats them as distinct.

Fix: quantize to a small tolerance (`1e-5` world units) before hashing and
comparing. Use the same quantization for both sides or values won't match
what was inserted.

### Subdivision scheme gotchas (SpectralSubdiv / OpenSubdiv)

- `SchemeFromToken`: empty / "none" / unknown -> `Scheme::None`. Most Nuke-
  sourced USD meshes have no explicit `subdivisionScheme` attribute, so they
  come through as None.
- `Refine` bails on `Scheme::None` even if `level > 0`. So setting a level
  override without also setting a scheme does nothing.
- Mitigation: when a level override is present but scheme resolves to None,
  force CatmullClark.
- Loop scheme requires all-triangle input. GeoCard / GeoSphere are quads.
- Level is clamped to `[1, 4]` internally. Knob range of 0-6 is misleading.

### CUDA buffer lifecycle during knob drags (free-during-kernel race)

A rapid knob drag triggers multiple `_validate` calls from Nuke in quick
succession. Each one re-extracts the scene, and if a checksummed input
(HDRI texels, material baseColor, etc.) changed, we `cudaFree` + realloc
the corresponding device buffer. If a previous `optixLaunch` for the
same Iop is still running on the default stream, the kernel reads freed
memory -> access violation -> GPU driver reset -> hard Nuke crash.

Mitigation in place: `cudaDeviceSynchronize()` at the top of
`SpectralGPU::BuildAccel`, before any cudaFree. Waits for any in-flight
launch to finish before we touch its device buffers. Small cost on the
happy path (immediate return); prevents the crash on the drag path.

Longer-term: per-Iop CUDA stream with events, so renders serialize on
events instead of a device-wide sync. Not yet done.

### Dev iteration: SPECTRAL_FAST_COMPILE env var

OptiX PTX compile is ~70s cold on our target kernel at `-O3`. For
iteration where you're recompiling often, set `SPECTRAL_FAST_COMPILE=1`
before launching Nuke -- we pass
`OPTIX_COMPILE_OPTIMIZATION_LEVEL_0` to the module, cutting cold
compile to ~10s. Runtime perf drops ~20-30%, so don't ship with it.
Cache hit (no source change) is ~140ms either way.

Also: **don't `printf` from device code**. A single `printf` in the
kernel balloons OptiX PTX compile from ~200ms warm to 80s+ because it
disables optimisations aggressively. If you need device-side debugging,
gate prints behind a launch-index check and remove before committing.

### Monte Carlo estimators: the missing `/pdf` divide

Any Monte Carlo estimator drawing samples from a non-uniform distribution
must divide by the sampling pdf. For direct lighting with importance
sampling, the estimator is:

    contribution = bsdf(L) * L_env(L) * cos(N,L) * misWeight / pdfLight

The `/pdfLight` is non-negotiable -- without it the estimator isn't
unbiased, and worse: the contribution is directly proportional to the
sampling density. Regions the sampler concentrates on (bright HDRI
pixels, the sun) get over-weighted exactly where the CDF was trying to
concentrate effort. Regions it doesn't concentrate on get zero.

We had this bug in both CPU (SpectralIntegrator.cpp:1735, :2121) and
GPU (SpectralGPUKernel.cu:1151) for a long time. It stayed invisible as
long as HDRI sampling was a uniform / cosine-weighted hemisphere --
the missing divide just produced a constant bias. When 2D CDF importance
sampling landed (Phase 15 GPU parity, 2026-04-22) and actually
concentrated samples on the HDRI's sun pixel, the missing divide
manifested as a hard terminator line across the sphere along the locus
where `pdfLight` transitioned from "huge" to "modest".

Diagnostic that nailed it: **blur the HDRI upstream**. If the terminator
line disappears, the pdf is spiky and the divide is missing (or wrong).
If it persists, look elsewhere.

Notes:
- Guard the divide with `pdfLight > 1e-10f` to avoid NaN / Inf at poles
  and at dark pixels where `lum * sinTheta` goes to zero.
- BSDF-IS / bounce-miss paths typically bake `1/pdfBsdf` into the path
  throughput as part of BSDF sampling, so those do *not* need an
  explicit divide at the miss site. Only light-IS / NEE branches need
  the explicit `/pdfLight`.
- Applying this fix makes every HDRI-lit render noticeably dimmer
  because the previous math was inflating contribution. Not a
  regression -- just unbiased now. Bump `hdri_intensity` defaults
  if the new baseline feels too dark across the board.

Rule for writing any new MC estimator (light sampling, volume phase
sampling, subsurface walk, photon gather with IS): write the `/pdf`
divide on the same line as the contribution. If you find yourself
typing `radiance += bsdf * L * misW` with no `/pdf` in sight, stop and
confirm the pdf is baked in elsewhere. Usually it isn't.

---

## Codebase conventions

### Logging

- `SLOG(...)` is the primary logging macro. Prints to stderr.
- Prefer logging over silent failure. Every fallback path, every skipped
  override, every registry miss deserves a log line.
- Logs live with the code -- they're how future-us debugs weird behaviour
  without re-reading the whole file.

### File layout

```
C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\src\
  SpectralRenderIop.cpp       # the main Iop -- scene read, render dispatch
  SpectralIntegrator.cpp      # CPU ray tracer
  SpectralGPUKernel.cu        # OptiX kernel
  SpectralGPU.cpp             # host-side OptiX pipeline
  Spectral<Thing>Op.{h,cpp}   # ShaderOp / GeomOp plugins for Nuke UI
  Hd<Thing>.{cpp,h}           # Hydra render delegate path (alternate frontend)
```

### Style

- Use `auto` where it improves readability, explicit types otherwise.
- Prefer single-file `.h` + `.cpp` pairs. No PCH.
- Warm/casual comments are fine and welcome. Cite references and papers when
  implementing literature (Wrenninge 2015, Wilkie 2014, etc.).
- No `using namespace std;` at file scope. `using namespace DD::Image;` in
  Op .cpp files is OK because they're entirely within Nuke-land.

---

## When asked to change something

1. **Read the relevant files before patching.** Don't guess from memory;
   the code has details that matter.
2. **Grep for related code paths.** Many features have both a CPU and a GPU
   path, both a main-mesh and a PointInstancer path, etc. Fixes typically
   need to land in all of them.
3. **Check whether the change affects the Iop output.** If yes, something
   needs to land in `append(Hash&)` too.
4. **Check whether the change involves a Nuke-managed lifecycle** (knob
   change, node disable, node delete). Each has its own gotcha.
5. **Prefer surgical patches over rewrites.** The code has accumulated
   known-good behaviour; preserve it.

## When the user reports "I change X and nothing happens"

First question is always: **is X in the hash?** Check `SpectralRenderIop::append`.
If the user says "it works once I scrub the timeline," that's diagnostic --
`frame()` is in the hash, so scrubbing invalidates the cache and forces re-
evaluation. The real fix is adding X to `append`.

## When the user reports "disabling the node doesn't disable its effect"

Check whether the node writes to any static registry. If yes, `RegisterParams`
(or equivalent) needs to erase the entry when `node_disabled()` is true, and
the consuming Iop needs to hash the registry.

---

## Things we haven't done yet but should

- Clean up registry entries on node destruction (currently leaks on rename/delete).
- Call `Iop::append(hash)` at the top of `SpectralRenderIop::append` to auto-
  hash all knobs. Considered 2026-04-22 but deferred: base-class append would
  also sweep viewport-preview knobs (`vdb_point_density`, `vdb_show_points`,
  `vdb_show_bbox`, `vp_*`), turning cheap viewport twiddles into render-cache
  invalidations. Quick smoke-test felt fine but we decided the current
  explicit hash list is cheap to maintain. Revisit only if a "toggle X does
  nothing" bug resurfaces in practice.
- HDRI intensity calibration post-MIS-fix. The `/pdfLight` bug fix
  (2026-04-22) makes every HDRI render substantially dimmer than pre-fix
  renders. After a few days of rendering at the new baseline, decide
  whether to bump default `hdri_intensity` by a constant factor (~2-3x
  suspected) so neutral scenes match expectations. Until then, users
  opening old scripts may need to manually push intensity up.
- HDRI Read-pipe "empty validate" detection. Theoretical race: upstream
  Read intermittently delivers all-zero texels on rapid knob drags,
  overwriting the GPU HDRI with zeros -> transient black frames. We
  tested for this on 2026-04-22 and could not reproduce visible
  black-frame flashes. Likely the GPU content-hash skip (today's work)
  already masks the effect. Left as a watch-note rather than a
  committed fix: if the symptom ever appears in practice, the plan is
  to detect avg==0 or first-N-texels==0 in SpectralRenderIop when
  constructing the dome light, skip both the dome update and the
  texture re-upload for that validate, and log "HDRI pipe empty on
  this validate, keeping previous frame".
- Unify the two HDRI dome construction sites in SpectralRenderIop.cpp
  (~line 7656 and ~line 7829) into one helper. Currently both need to be
  updated in lockstep whenever dome behaviour changes, which has already
  caused divergence bugs.
- Per-Iop CUDA stream with events instead of the current device-wide
  `cudaDeviceSynchronize` at BuildAccel entry. Proper fix for the
  free-during-kernel race that currently uses a sync as mitigation.
