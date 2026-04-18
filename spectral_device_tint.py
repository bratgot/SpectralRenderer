# SpectralRenderer -- spectral_device_tint.py
#
# Drives the built-in tile_color knob on every SpectralRender node from its
# device_mode setting, so each node tile carries a subtle colour cue for
# CPU / GPU / AUTO without any DAG overlay or Qt hackery.
#
# Colours are muted and sit comfortably next to Nuke's default UI greys.
# User-set tile_color values are overridden -- this is the whole point of
# the feature. If you need a custom tile_color on a SpectralRender, either
# comment out the import in menu.py or change _TILE_COLORS below.

import nuke

NODE_CLASS  = "SpectralRender"
DEVICE_KNOB = "device_mode"   # 0=cpu, 1=gpu, 2=auto

# Packed RGBA ints: 0xRRGGBBAA. Same palette as the earlier Qt attempt --
# desaturated, similar luminance, readable at tile size.
_TILE_COLORS = {
    0: 0x4A6C8CFF,  # CPU  -- soft slate blue
    1: 0x4A8C6CFF,  # GPU  -- soft teal-green
    2: 0x8C764AFF,  # AUTO -- warm bronze-grey
}


def _apply_tile_color(node):
    if node is None:
        return
    try:
        dev = int(node[DEVICE_KNOB].value())
    except Exception:
        return
    color = _TILE_COLORS.get(dev)
    if color is None:
        return
    try:
        tc = node['tile_color']
        if tc.value() != color:
            tc.setValue(color)
    except Exception:
        pass


def _on_create():
    # Fires when a SpectralRender is created, including during script load.
    _apply_tile_color(nuke.thisNode())


def _on_knob_changed():
    # addKnobChanged fires for every knob on the node class; filter to ours.
    try:
        if nuke.thisKnob().name() == DEVICE_KNOB:
            _apply_tile_color(nuke.thisNode())
    except Exception:
        pass


def apply_all():
    """Re-tint every existing SpectralRender. Safe to call repeatedly."""
    try:
        for node in nuke.allNodes(NODE_CLASS):
            _apply_tile_color(node)
    except Exception:
        pass


nuke.addOnCreate(_on_create, nodeClass=NODE_CLASS)
nuke.addKnobChanged(_on_knob_changed, nodeClass=NODE_CLASS)

# Catch any nodes already present at module load time (e.g. a reload during
# development, or nodes instantiated before this module was imported).
apply_all()
