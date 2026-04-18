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

---

## C++ / build gotchas

### MSVC lambda capture quirk

MSVC requires `constexpr` locals to be explicitly captured in lambdas. GCC/Clang
let this slide. Either:
- Use `[&]` / `[=]` default capture, or
- Inline the constant value into the lambda body directly.

The second is preferred for small constants -- no capture overhead, no ambiguity.

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

- Proper prim-path registration in `SpectralMeshPropertiesOp::processScenegraph`
  (instead of the current `size() == 1` fallback).
- Clean up registry entries on node destruction (currently leaks on rename/delete).
- Wire up `doubleSided`, `castsShadows`, `visible`, `orientation`, `purpose`
  from SpectralMeshProperties -- currently registered but unused.
- Call `Iop::append(hash)` at the top of `SpectralRenderIop::append` to auto-
  hash all knobs (would pre-empt a whole class of "toggle doesn't work" bugs).
