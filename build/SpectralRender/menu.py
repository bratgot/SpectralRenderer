import nuke

# Force-load the SpectralRender plugin which contains both
# SpectralRender and SpectralSurface node classes
nuke.load("SpectralRender")

# Add to toolbar
toolbar = nuke.toolbar("Nodes")
spectralMenu = toolbar.addMenu("SpectralRenderer", icon="Render.png")
spectralMenu.addCommand("SpectralRender", "nuke.createNode('SpectralRender')")
spectralMenu.addCommand("SpectralSurface", "nuke.createNode('SpectralSurface')")
