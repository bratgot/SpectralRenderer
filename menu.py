# SpectralRenderer — Nuke menu registration
# Place alongside SpectralRender.dll and icons in your NUKE_PATH

import nuke
import os

# Ensure icon directory is on plugin path
_dir = os.path.dirname(__file__)
if _dir and _dir not in nuke.pluginPath():
    nuke.pluginAddPath(_dir)

toolbar = nuke.menu("Nodes")

# Spectral submenu under 3D
m = toolbar.addMenu("3D/Spectral", icon="SpectralRender.png")
m.addCommand("SpectralRender",         "nuke.createNode('SpectralRender')",         icon="SpectralRender.png")
m.addCommand("SpectralSurface",        "nuke.createNode('SpectralSurface')",        icon="SpectralSurface.png")
m.addCommand("SpectralVolumeMaterial", "nuke.createNode('SpectralVolumeMaterial')", icon="SpectralVolumeMaterial.png")
m.addCommand("-", "", "")  # separator
m.addCommand("SpectralVDBRead", "nuke.createNode('SpectralVDBRead')", icon="SpectralVDBRead.png")
m.addCommand("-", "", "")  # separator
m.addCommand("SpectralEnvLight",    "nuke.createNode('SpectralEnvLight')",    icon="SpectralEnvLight.png")
m.addCommand("SpectralStudioLight", "nuke.createNode('SpectralStudioLight')", icon="SpectralStudioLight.png")

# Top-level Tab access
toolbar.addCommand("Spectral/SpectralRender",         "nuke.createNode('SpectralRender')")
toolbar.addCommand("Spectral/SpectralSurface",        "nuke.createNode('SpectralSurface')")
toolbar.addCommand("Spectral/SpectralVolumeMaterial", "nuke.createNode('SpectralVolumeMaterial')")
toolbar.addCommand("Spectral/SpectralVDBRead",        "nuke.createNode('SpectralVDBRead')")
toolbar.addCommand("Spectral/SpectralEnvLight",       "nuke.createNode('SpectralEnvLight')")
toolbar.addCommand("Spectral/SpectralStudioLight",    "nuke.createNode('SpectralStudioLight')")
