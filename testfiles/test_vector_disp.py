#!/usr/bin/env python3
"""
SpectralRenderer — Vector Displacement test scene (beta)

Creates:
1. A simple USD scene with a subdivided plane
2. A vector displacement EXR map (requires OpenEXR or numpy)

The displacement map has RGB channels where:
  R = X offset (tangent direction)
  G = Y offset (bitangent direction)
  B = Z offset (normal direction)

Usage:
    python test_vector_disp.py
    → Creates test_vector_disp.usda and test_vector_disp.exr
"""

import math
import struct
import os


def write_displacement_hdr(filename="test_vector_disp.hdr", res=128):
    """Write a simple HDR file with vector displacement pattern.

    Uses Radiance HDR format (simple to write, widely supported).
    Creates a wavy displacement pattern in RGB.
    """
    pixels = []
    for y in range(res):
        for x in range(res):
            u = x / float(res - 1)
            v = y / float(res - 1)

            # Procedural vector displacement: wavy surface
            freq = 3.0
            r = math.sin(u * freq * math.pi) * 0.3           # X tangent wave
            g = math.cos(v * freq * math.pi) * 0.2           # Y bitangent wave
            b = (math.sin(u * freq * 2 * math.pi) *
                 math.cos(v * freq * 2 * math.pi)) * 0.5 + 0.5  # Z normal displacement

            # Bias to 0.5 center (midpoint=0.5)
            r = r * 0.5 + 0.5
            g = g * 0.5 + 0.5

            pixels.append((r, g, b))

    # Write Radiance HDR format
    with open(filename, "wb") as f:
        header = f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {res} +X {res}\n"
        f.write(header.encode('ascii'))

        for r, g, b in pixels:
            # Convert to RGBE
            mx = max(r, g, b)
            if mx < 1e-32:
                f.write(b'\x00\x00\x00\x00')
            else:
                mantissa, exp = math.frexp(mx)
                mult = mantissa * 256.0 / mx
                f.write(bytes([
                    max(0, min(255, int(r * mult))),
                    max(0, min(255, int(g * mult))),
                    max(0, min(255, int(b * mult))),
                    max(0, min(255, exp + 128))
                ]))

    print(f"Created {filename}")
    print(f"  {res}x{res} vector displacement map (HDR)")
    print(f"  Wavy pattern: R=tangent, G=bitangent, B=normal")


def write_usda(filename="test_vector_disp.usda", subdivisions=4):
    """Write a USD scene with a plane for displacement testing."""
    # Simple 1x1 quad (will be subdivided by OpenSubdiv)
    with open(filename, "w") as f:
        f.write('#usda 1.0\n')
        f.write('(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\n\n')
        f.write('def Xform "World"\n{\n')

        # Subdivided plane
        f.write('    def Mesh "dispPlane"\n    {\n')
        f.write('        float3[] points = [\n')
        # 5x5 grid of points
        n = 5
        pts = []
        for iz in range(n):
            for ix in range(n):
                x = (ix / (n - 1.0)) * 4.0 - 2.0
                z = (iz / (n - 1.0)) * 4.0 - 2.0
                pts.append(f"({x:.2f}, 0.00, {z:.2f})")
        f.write("            " + ", ".join(pts) + "\n")
        f.write('        ]\n')

        # Quads
        fvc = []
        fvi = []
        for iz in range(n - 1):
            for ix in range(n - 1):
                i0 = iz * n + ix
                i1 = iz * n + ix + 1
                i2 = (iz + 1) * n + ix + 1
                i3 = (iz + 1) * n + ix
                fvc.append(4)
                fvi.extend([i0, i1, i2, i3])
        f.write(f'        int[] faceVertexCounts = {fvc}\n')
        f.write(f'        int[] faceVertexIndices = {fvi}\n')

        # UVs
        uvs = []
        for iz in range(n - 1):
            for ix in range(n - 1):
                u0, v0 = ix / (n - 1.0), iz / (n - 1.0)
                u1, v1 = (ix + 1) / (n - 1.0), iz / (n - 1.0)
                u2, v2 = (ix + 1) / (n - 1.0), (iz + 1) / (n - 1.0)
                u3, v3 = ix / (n - 1.0), (iz + 1) / (n - 1.0)
                uvs.extend([f"({u0:.3f},{v0:.3f})", f"({u1:.3f},{v1:.3f})",
                            f"({u2:.3f},{v2:.3f})", f"({u3:.3f},{v3:.3f})"])
        f.write(f'        texCoord2f[] primvars:st = [{", ".join(uvs)}] (\n')
        f.write('            interpolation = "faceVarying"\n')
        f.write('        )\n')

        f.write(f'        uniform token subdivisionScheme = "catmullClark"\n')
        f.write('    }\n\n')

        # Light
        f.write('    def DistantLight "key"\n    {\n')
        f.write('        float inputs:intensity = 3.0\n')
        f.write('        float3 xformOp:rotateXYZ = (-45, 30, 0)\n')
        f.write('        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]\n')
        f.write('    }\n')

        f.write('}\n')

    print(f"Created {filename}")
    print(f"  Subdivided plane ({n}x{n} base grid, catmullClark)")
    print(f"  Use SpectralSurface: set map mode to 'displacement',")
    print(f"  disp type to 'vector tangent (beta)',")
    print(f"  connect test_vector_disp.hdr to disp/bump pipe")


if __name__ == "__main__":
    write_usda()
    write_displacement_hdr()
    print("\n--- Setup in Nuke ---")
    print("1. ReadGeo2 node → load test_vector_disp.usda")
    print("2. SpectralSurface → set preset 'custom'")
    print("3. Map section: mode = displacement")
    print("4. disp type = 'vector tangent (beta)'")
    print("5. disp scale = 0.5, midpoint = 0.5")
    print("6. Read node → load test_vector_disp.hdr")
    print("7. Connect Read to SpectralSurface disp/bump pipe")
    print("8. Connect SpectralSurface + ReadGeo2 to SpectralRender")
