# SpectralRenderer — Nuke menu registration
# Place this file alongside SpectralRender.dll in your NUKE_PATH
# or in ~/.nuke/

import nuke

# Add to toolbar
toolbar = nuke.menu("Nodes")

# Create Spectral submenu under 3D
spectralMenu = toolbar.addMenu("3D/Spectral", icon="Render3D.png")
spectralMenu.addCommand("SpectralRender", "nuke.createNode('SpectralRender')", icon="Render3D.png")
spectralMenu.addCommand("SpectralSurface", "nuke.createNode('SpectralSurface')", icon="Material.png")

# Also add to top-level for quick Tab access
toolbar.addCommand("Spectral/SpectralRender", "nuke.createNode('SpectralRender')")
toolbar.addCommand("Spectral/SpectralSurface", "nuke.createNode('SpectralSurface')")
