#!/usr/bin/env python3
"""
SpectralRenderer — PointInstancer test scene (beta)

Creates a USD file with a PointInstancer that scatters
small spheres in a grid pattern. Load in Nuke 17 with
SpectralRender to test instancing support.

Usage:
    python test_instancer.py
    → Creates test_instancer.usda in current directory
"""

import math

def make_sphere_verts(radius=0.3, slices=8, stacks=6):
    """Generate a simple UV sphere as points + face indices."""
    points = []
    fvc = []     # faceVertexCounts
    fvi = []     # faceVertexIndices

    # Top cap vertex
    points.append((0, radius, 0))

    # Middle rings
    for i in range(1, stacks):
        phi = math.pi * i / stacks
        for j in range(slices):
            theta = 2.0 * math.pi * j / slices
            x = radius * math.sin(phi) * math.cos(theta)
            y = radius * math.cos(phi)
            z = radius * math.sin(phi) * math.sin(theta)
            points.append((x, y, z))

    # Bottom cap vertex
    points.append((0, -radius, 0))

    # Top cap triangles
    for j in range(slices):
        j1 = (j + 1) % slices
        fvc.append(3)
        fvi.extend([0, 1 + j, 1 + j1])

    # Middle quads
    for i in range(stacks - 2):
        for j in range(slices):
            j1 = (j + 1) % slices
            base = 1 + i * slices
            fvc.append(4)
            fvi.extend([base + j, base + slices + j, base + slices + j1, base + j1])

    # Bottom cap triangles
    bottom = len(points) - 1
    base = 1 + (stacks - 2) * slices
    for j in range(slices):
        j1 = (j + 1) % slices
        fvc.append(3)
        fvi.extend([bottom, base + j1, base + j])

    return points, fvc, fvi


def write_usda(filename="test_instancer.usda", grid=5, spacing=1.5):
    """Write a USD file with a prototype sphere and a grid PointInstancer."""
    points, fvc, fvi = make_sphere_verts(radius=0.25)

    # Instance positions: grid x grid x 1 layer
    positions = []
    indices = []
    for iz in range(grid):
        for ix in range(grid):
            x = (ix - grid / 2.0 + 0.5) * spacing
            z = (iz - grid / 2.0 + 0.5) * spacing
            y = 0.0
            positions.append((x, y, z))
            indices.append(0)  # all use prototype 0

    with open(filename, "w") as f:
        f.write('#usda 1.0\n')
        f.write('(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\n\n')
        f.write('def Xform "World"\n{\n')

        # Ground plane
        f.write('    def Mesh "ground"\n    {\n')
        f.write('        float3[] points = [(-5,0,-5),(5,0,-5),(5,0,5),(-5,0,5)]\n')
        f.write('        int[] faceVertexCounts = [4]\n')
        f.write('        int[] faceVertexIndices = [0,1,2,3]\n')
        f.write('    }\n\n')

        # Distant light
        f.write('    def DistantLight "sun"\n    {\n')
        f.write('        float inputs:intensity = 3.0\n')
        f.write('        float3 xformOp:rotateXYZ = (-45, 30, 0)\n')
        f.write('        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]\n')
        f.write('    }\n\n')

        # Prototype sphere (inside Prototypes scope)
        f.write('    def Scope "Prototypes"\n    {\n')
        f.write('        def Mesh "sphere"\n        {\n')
        pts_str = ", ".join(f"({p[0]:.4f},{p[1]:.4f},{p[2]:.4f})" for p in points)
        f.write(f'            float3[] points = [{pts_str}]\n')
        f.write(f'            int[] faceVertexCounts = {fvc}\n')
        f.write(f'            int[] faceVertexIndices = {fvi}\n')
        f.write('        }\n')
        f.write('    }\n\n')

        # PointInstancer
        f.write('    def PointInstancer "scatter"\n    {\n')
        pos_str = ", ".join(f"({p[0]:.3f},{p[1]:.3f},{p[2]:.3f})" for p in positions)
        f.write(f'        point3f[] positions = [{pos_str}]\n')
        f.write(f'        int[] protoIndices = {indices}\n')
        f.write('        rel prototypes = [</World/Prototypes/sphere>]\n')
        f.write('    }\n')

        f.write('}\n')

    print(f"Created {filename}")
    print(f"  {len(positions)} instances of sphere prototype")
    print(f"  {grid}x{grid} grid, spacing {spacing}")
    print(f"  Load in Nuke: ReadGeo2 → SpectralRender")


if __name__ == "__main__":
    write_usda()
