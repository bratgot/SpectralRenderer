# SpectralRenderer — plugin loader
# Place this file alongside SpectralRender.dll in your NUKE_PATH
# or add this line to your ~/.nuke/init.py:
#   nuke.pluginAddPath('/path/to/SpectralRender')

import nuke
import os

# SpectralRenderer requires Nuke 17+ (USD/USG scene graph API)
if nuke.NUKE_VERSION_MAJOR < 17:
    pass  # silently skip — plugin won't load in older Nuke
else:
    # Add the directory containing this init.py to the plugin path
    plugin_dir = os.path.dirname(__file__)
    if plugin_dir:
        nuke.pluginAddPath(plugin_dir)

    # If VDB DLLs are in a lib/ subfolder, add to system PATH
    lib_dir = os.path.join(plugin_dir, "lib")
    if os.path.isdir(lib_dir):
        os.environ["PATH"] = lib_dir + os.pathsep + os.environ.get("PATH", "")

    # Force-load SpectralRender.dll so ALL node classes register at startup
    # (SpectralRender, SpectralSurface, SpectralVDBRead, SpectralVolumeMaterial)
    # Without this, only SpectralRender registers on first use — the others
    # show "Unknown command" until a SpectralRender node is created.
    nuke.load("SpectralRender")
