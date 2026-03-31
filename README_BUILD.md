# HdSpectral — Phase 1 Build Guide
## Nuke 17.0 · Visual Studio 2022 · Windows 11

---

## Prerequisites

| Tool | Version | Where |
|------|---------|--------|
| Visual Studio 2022 | v143 toolchain | visualstudio.microsoft.com |
| CMake | ≥ 3.22 | cmake.org |
| Nuke 17.0 | any v build | foundry.com |
| CUDA Toolkit | ≥ 12.0 (for Phase 3) | developer.nvidia.com |

---

## Step 1 — Environment variables

Open **x64 Native Tools Command Prompt for VS 2022** and set:

```bat
:: Point PXR at the Nuke plugin output directory (set after build)
:: We set this BEFORE the build to avoid a chicken-and-egg problem with
:: the plugInfo.json path.

set NUKE_ROOT=C:\Program Files\Nuke17.0v6
```

Replace `v6` with your actual Nuke build number (check the install folder name).

---

## Step 2 — Configure

```bat
cd /d C:\dev\HdSpectral

cmake -S . -B build ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -D NUKE_ROOT="C:/Program Files/Nuke17.0v6" ^
  -D CMAKE_BUILD_TYPE=Release
```

CMake will print which PXR libs it found.  You need at minimum:
`tf`, `gf`, `vt`, `sdf`, `hd`, `usd`, `usdGeom`, `usdImaging` to be found.

If any are missing, locate them manually under `%NUKE_ROOT%` and add:
```
-D pxr_DIR="C:/Program Files/Nuke17.0v6"
```

---

## Step 3 — Build

```bat
cmake --build build --config Release --parallel
```

Output:
```
build/HdSpectral/
    HdSpectral.dll
    HdSpectral.lib
    resources/
        plugInfo.json
```

---

## Step 4 — Register with Nuke

Set `PXR_PLUGINPATH_NAME` to the directory containing `plugInfo.json`'s **parent**
(i.e. the directory containing the `HdSpectral/` folder):

```bat
:: In your nuke launch script or system environment:
set PXR_PLUGINPATH_NAME=C:\dev\HdSpectral\build

:: You also need the DLL on PATH so Windows can load it
set PATH=C:\dev\HdSpectral\build\HdSpectral;%PATH%
```

Or create a `nuke_spectral.bat` launcher:
```bat
@echo off
set NUKE_ROOT=C:\Program Files\Nuke17.0v6
set PXR_PLUGINPATH_NAME=C:\dev\HdSpectral\build
set PATH=%CD%\build\HdSpectral;%NUKE_ROOT%;%PATH%
"%NUKE_ROOT%\Nuke17.0.exe" %*
```

---

## Step 5 — Verify registration

Start Nuke, open a 3D workspace, create a GeoImport node, connect to a
ScanlineRender-style node, and look for **"Spectral (CPU)"** in the
renderer dropdown.  If it's not there:

1. Check the Nuke console for TF_DEBUG messages — add `TF_DEBUG=HD_RENDERER_PLUGIN`
   to your environment.
2. Run from command line with `NUKE_ROOT\Nuke17.0 --tg` to see plugin load messages.
3. Verify `plugInfo.json` path is correct:
   ```
   build\
     HdSpectral\           ← PXR_PLUGINPATH_NAME points HERE (the parent)
       HdSpectral.dll
       resources\
         plugInfo.json     ← must exist
   ```

---

## Step 6 — First render

1. Create a **GeoCube** node (or import a USD file via GeoImport)
2. Create a **RenderSettings** node, set renderer to **Spectral (CPU)**
3. Create a **RenderProduct** node bound to a **RenderVar** with token `color`
4. Click **Render** — you should see world-space normals as RGB colour
5. Confirm the blue-sky gradient appears for pixels that miss geometry

---

## Debugging

```bat
:: Enable Hydra plugin debug output
set TF_DEBUG=HD_RENDERER_PLUGIN HD_DIRTY_LIST HD_TASK_CONTEXT

:: Enable our own delegate messages
set TF_DEBUG=HDSPECTRAL

:: Dump USD stage as it enters Hydra
set TF_DEBUG=USDIMAGING_UPDATES
```

---

## What works in Phase 1

- [x] Hydra delegate registered and appearing in Nuke's renderer list
- [x] USD mesh prims Sync'd (points, topology, normals, transform)
- [x] Arbitrary polygon meshes triangulated via HdMeshUtil
- [x] Smooth normals computed when none are authored
- [x] Möller–Trumbore brute-force ray–triangle intersection
- [x] Normal-as-colour shading (world space, remapped [0,1])
- [x] Sky gradient background
- [x] Multi-core pixel loop via std::execution::par_unseq
- [x] AOV buffer wired to Nuke "color" render var
- [x] Visibility toggling

## Not yet in Phase 1 (see roadmap)

- [ ] BVH acceleration (Embree 4) — Phase 2
- [ ] OpenSubdiv tessellation — Phase 2
- [ ] Spectral sampling (hero wavelength) — Phase 2
- [ ] OptiX GPU path — Phase 3
- [ ] PBR materials (spectral Disney BSDF) — Phase 4
- [ ] Spectral lights / CIE illuminants — Phase 4

---

## Common build errors

| Error | Fix |
|-------|-----|
| `DDImage.lib not found` | Check `NUKE_ROOT` points to the actual Nuke binary folder, not a subdirectory |
| `hd.lib not found` | On some Nuke 17 builds the lib is `hd_d.lib` in debug — use Release build |
| `unresolved external: __imp_TfRegistryManager` | You need to link `tf.lib` — check PXR_LIBS cmake output |
| `NOMINMAX` / `min max` redefinition | Already handled in CMakeLists — ensure you're using our CMake, not a hand-rolled one |
| Plugin not appearing in Nuke | `plugInfo.json` path wrong — see Step 5 above |
