# SpectralRenderer -- menu.py
# Adds all SpectralRenderer nodes to the Nuke toolbar.

import nuke

if nuke.NUKE_VERSION_MAJOR >= 17:

    toolbar = nuke.toolbar("Nodes")
    m = toolbar.addMenu("SpectralRenderer", icon="SpectralRender.png")

    # --- Rendering ---
    m.addCommand("SpectralRender", "nuke.createNode('SpectralRender')", icon="SpectralRender.png")

    m.addSeparator()

    # --- Materials ---
    m.addCommand("Materials/SpectralSurface", "nuke.createNode('SpectralSurface')", icon="SpectralSurface.png")
    m.addCommand("Materials/SpectralWireframe", "nuke.createNode('SpectralWireframe')", icon="SpectralWireframe.png")
    m.addCommand("Materials/SpectralShadowCatcher", "nuke.createNode('SpectralShadowCatcher')", icon="SpectralShadowCatcher.png")
    m.addCommand("Materials/SpectralVolumeMaterial", "nuke.createNode('SpectralVolumeMaterial')", icon="SpectralVolumeMaterial.png")

    m.addSeparator()

    # --- Geometry ---
    m.addCommand("Geometry/SpectralMeshProperties", "nuke.createNode('SpectralMeshProperties')")

    m.addSeparator()

    # --- Scene ---
    m.addCommand("Scene/SpectralVDBRead", "nuke.createNode('SpectralVDBRead')", icon="SpectralVDBRead.png")
    m.addCommand("Scene/SpectralVolMerge", "nuke.createNode('SpectralVolMerge')", icon="SpectralVolMerge.png")

    m.addSeparator()

    # --- Lighting ---
    m.addCommand("Lighting/SpectralEnvLight", "nuke.createNode('SpectralEnvLight')", icon="SpectralEnvLight.png")
    m.addCommand("Lighting/SpectralStudioLight", "nuke.createNode('SpectralStudioLight')", icon="SpectralStudioLight.png")

    # Tile colouring by device_mode (CPU / GPU / AUTO) -- registers callbacks.
    try:
        import spectral_device_tint  # noqa: F401
    except Exception as e:
        nuke.tprint("SpectralRenderer: device tint failed to load: %s" % e)
