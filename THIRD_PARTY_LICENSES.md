# Third-Party Licenses

SpectralRenderer uses the following open-source libraries and resources.
All licenses are compatible with commercial VFX studio use.

---

## Intel Embree 4
**License:** Apache License 2.0
**URL:** https://github.com/embree/embree
**Used for:** CPU ray tracing (BVH construction and traversal)
**VFX studio use:** Permitted. Apache 2.0 allows commercial use, modification,
and distribution. Attribution required in documentation (this file).

---

## Pixar OpenSubdiv 3.6
**License:** Apache License 2.0 (Modified)
**URL:** https://github.com/PixarAnimationStudios/OpenSubdiv
**Used for:** Catmull-Clark, Loop, and bilinear subdivision surfaces with
crease edges and face-varying UV refinement.
**VFX studio use:** Permitted. Pixar's license allows commercial use.
Standard in VFX pipelines (used by Presto, Hydra, USD).

---

## Pixar Universal Scene Description (USD)
**License:** Apache License 2.0 (Modified)
**URL:** https://github.com/PixarAnimationStudios/OpenUSD
**Used for:** Scene graph, geometry, materials, lights, cameras, and
the Hydra rendering framework (HdSpectral render delegate).
**VFX studio use:** Permitted. Industry standard. Bundled with Nuke 17.

---

## NVIDIA OptiX 8.1
**License:** NVIDIA Software License (proprietary, free to use)
**URL:** https://developer.nvidia.com/optix
**Used for:** GPU ray tracing (RTX hardware acceleration), AI denoiser.
**VFX studio use:** Permitted. Free for commercial use. Requires NVIDIA GPU.
Redistribution of OptiX SDK headers/libs subject to NVIDIA EULA.
No royalties. No per-seat licensing.

---

## NVIDIA CUDA Toolkit 12.6
**License:** NVIDIA CUDA Toolkit EULA (proprietary, free to use)
**URL:** https://developer.nvidia.com/cuda-toolkit
**Used for:** GPU kernel compilation and execution.
**VFX studio use:** Permitted. Free for commercial use on NVIDIA hardware.

---

## stb_image
**License:** MIT License / Public Domain (dual-licensed)
**URL:** https://github.com/nothings/stb
**Used for:** Loading texture images (PNG, JPG, HDR, EXR).
**VFX studio use:** Permitted. No restrictions. Single-header library.

---

## Foundry Nuke 17 NDK (DDImage)
**License:** Foundry commercial license
**URL:** https://www.foundry.com/products/nuke
**Used for:** Nuke plugin API (Iop, RenderIop, ShaderOp, Knobs, Channels).
**VFX studio use:** Requires valid Nuke license. Plugin development permitted
under Nuke SDK terms. SpectralRenderer is a plugin, not a modification of Nuke.

---

## CIE 1931 Standard Observer Data
**License:** Public domain (international standard)
**Used for:** Spectral-to-XYZ colour matching functions (380-780nm, 5nm intervals).
**VFX studio use:** No restrictions. Published by the International Commission
on Illumination (CIE) in 1931.

---

## Palik Handbook of Optical Constants
**License:** Published scientific data (fair use for material parameters)
**Used for:** Complex refractive index (n, k) values for metals
(Au, Cu, Ag, Al, Fe, Ti) used in Fresnel conductor calculations.
**VFX studio use:** Material property values are factual data, not copyrightable.
Used as numerical constants in the renderer.

---

## EPFL Realistic Graphics Lab (RGL) Material Database
**License:** Creative Commons (research use, see individual entries)
**URL:** https://rgl.epfl.ch/materials
**Used for:** Reference for measured BRDF material approximations.
SpectralRenderer does not include or redistribute the measured data.
Preset values are artist approximations of measured appearance.
**VFX studio use:** Permitted. No RGL data is included in the binary.

---

## Summary for VFX Studios

All dependencies are compatible with commercial VFX production:

| Library        | License        | Cost  | Commercial OK |
|----------------|----------------|-------|---------------|
| SpectralRenderer | MIT           | Free  | Yes           |
| Embree 4       | Apache 2.0     | Free  | Yes           |
| OpenSubdiv     | Apache 2.0     | Free  | Yes           |
| USD / Hydra    | Apache 2.0     | Free  | Yes           |
| OptiX 8.1      | NVIDIA EULA    | Free  | Yes           |
| CUDA 12.6      | NVIDIA EULA    | Free  | Yes           |
| stb_image      | MIT / PD       | Free  | Yes           |
| Nuke NDK       | Foundry EULA   | Licensed | Yes (plugin) |

No GPL or LGPL dependencies. No copyleft. No royalties.
Safe for internal studio tools and commercial production.
