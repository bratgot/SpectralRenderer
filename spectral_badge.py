# SpectralRenderer -- spectral_badge.py
#
# Subtle CPU / GPU / AUTO badge for SpectralRender nodes in the DAG.
#
# Paints a small rounded-rect pill on the top-right corner of each
# SpectralRender node tile indicating the current device_mode. Hides below
# a minimum zoom so it stays unobtrusive when surveying a large graph.
#
# Implementation:
#   - Top-level frameless translucent QWidget tracking each DAG's global
#     geometry. A child/sibling overlay doesn't work because the DAG canvas
#     is drawn with OpenGL and the GL surface paints over sibling Qt widgets
#     regardless of raise_() Z-order. A separate OS-level window sidesteps
#     that entirely.
#   - Node positions derived from node.xpos()/ypos()/screenWidth()/screenHeight(),
#     mapped to widget pixels via nuke.zoom() and nuke.center().
#   - Event filter on the DAG (and its top-level window) schedules debounced
#     repaints + geometry follow-ups so the badges track pan/zoom/move/resize.
#
# Known limitations:
#   - nuke.zoom() and nuke.center() report the *active* DAG's transform. If
#     you have multiple floating DAG panels, inactive ones will draw badges
#     using the active panel's transform (wrong positions). Single-DAG is the
#     common case so we accept this.
#   - DAG widget lookup matches objectName 'DAG', 'DAG.1', ... and class
#     'DAGNukeWindow'. If Foundry renames internals, update _find_dag_widgets.

import nuke

try:
    from PySide6.QtCore import QEvent, QPoint, Qt, QRectF, QTimer
    from PySide6.QtGui import QColor, QFont, QFontMetrics, QPainter
    from PySide6.QtWidgets import QApplication, QWidget
except ImportError:
    from PySide2.QtCore import QEvent, QPoint, Qt, QRectF, QTimer
    from PySide2.QtGui import QColor, QFont, QFontMetrics, QPainter
    from PySide2.QtWidgets import QApplication, QWidget


NODE_CLASS  = "SpectralRender"
DEVICE_KNOB = "device_mode"    # 0=cpu, 1=gpu, 2=auto
MIN_ZOOM    = 0.55             # below this, badges hide -- unreadable anyway

# Set True to paint a magenta border + corner crosshairs around each overlay
# window. Useful for confirming that the overlay is sized / positioned /
# z-ordered correctly. Flip back to False once badges are showing normally.
DEBUG = False

# Muted, desaturated palette. Reads as professional at small sizes and
# sits alongside Nuke's default UI greys without clashing.
_PALETTE = {
    0: QColor( 74, 108, 140),  # CPU  -- soft slate blue
    1: QColor( 74, 140, 108),  # GPU  -- soft teal-green
    2: QColor(140, 118,  74),  # AUTO -- warm bronze-grey
}
_LABELS = {0: "CPU", 1: "GPU", 2: "AUTO"}
_TEXT   = QColor(232, 236, 240)
_SHADOW = QColor(  0,   0,   0, 90)


import re

_DAG_OBJ_RE = re.compile(r"^DAG(\.\d+)?$")  # "DAG", "DAG.1", "DAG.2", ...


def _find_dag_widgets(verbose=False):
    """
    Return the DAG canvases we should overlay.

    Nuke exposes two Qt widgets per DAG pane at identical global geometry:
    an outer 'DAG_Window' and an inner 'DAGNukeWindow'. We dedupe by global
    rect and prefer the inner canvas -- its events are the ones we actually
    care about for pan/zoom tracking.
    """
    raw = []
    for w in QApplication.allWidgets():
        try:
            obj = w.objectName() or ""
            cls = w.metaObject().className() or ""
            if not w.isVisible():
                continue
        except Exception:
            continue
        if _DAG_OBJ_RE.match(obj) or cls == "DAGNukeWindow":
            raw.append((w, obj, cls))
            if verbose:
                nuke.tprint(
                    "SpectralBadge: DAG candidate  obj=%r  cls=%r  size=%dx%d"
                    % (obj, cls, w.width(), w.height())
                )

    # Dedupe by global top-left + size; prefer DAGNukeWindow when colliding.
    best = {}
    for w, obj, cls in raw:
        try:
            tl = w.mapToGlobal(QPoint(0, 0))
            key = (tl.x(), tl.y(), w.width(), w.height())
        except Exception:
            continue
        if key not in best:
            best[key] = (w, cls)
        else:
            prev_cls = best[key][1]
            if cls == "DAGNukeWindow" and prev_cls != "DAGNukeWindow":
                best[key] = (w, cls)

    return [w for (w, _cls) in best.values()]


def diagnose():
    """
    Print everything useful for debugging why badges aren't showing.
    Run from Nuke's Script Editor:
        from spectral_badge import diagnose; diagnose()
    """
    nuke.tprint("=== SpectralBadge diagnose ===")
    hits = 0
    for w in QApplication.allWidgets():
        try:
            obj = w.objectName() or ""
            cls = w.metaObject().className() or ""
        except Exception:
            continue
        if "DAG" in obj or "DAG" in cls or "Graph" in cls or "Node" in cls:
            nuke.tprint(
                "  candidate  obj=%r  cls=%r  visible=%s  size=%dx%d"
                % (obj, cls, w.isVisible(), w.width(), w.height())
            )
            hits += 1
    if hits == 0:
        nuke.tprint("  (no DAG-ish widgets found at all)")
    nuke.tprint("Installed overlays: %d" % len(_OVERLAYS))
    for o in _OVERLAYS:
        p = o.parent()
        po = p.objectName() if p else "<none>"
        nuke.tprint(
            "  overlay parent=%r  visible=%s  geom=%dx%d@(%d,%d)"
            % (po, o.isVisible(), o.width(), o.height(), o.x(), o.y())
        )
    try:
        nuke.tprint(
            "nuke.zoom()=%s  nuke.center()=%s  SpectralRender count=%d"
            % (nuke.zoom(), nuke.center(), len(nuke.allNodes(NODE_CLASS)))
        )
    except Exception as e:
        nuke.tprint("  (nuke.zoom/center/allNodes unavailable: %s)" % e)


class SpectralBadgeOverlay(QWidget):
    """
    Top-level frameless translucent window that tracks a DAG widget's
    global geometry and paints badges over it.

    Why a top-level window rather than a child widget: the DAG canvas is
    rendered via OpenGL, and a GL surface paints after sibling Qt widgets
    regardless of Z-order, so a child overlay is invisible in practice.
    A separate top-level window isn't subject to that occlusion.
    """

    def __init__(self, dag):
        # Parent to the main Nuke window so Windows establishes a proper
        # owner relationship -- otherwise an orphan Qt.Tool window ends up
        # below Nuke's main window in the Win32 z-order and is invisible.
        nuke_main = dag.window()
        super().__init__(
            nuke_main,
            Qt.Tool
            | Qt.FramelessWindowHint
            | Qt.NoDropShadowWindowHint
            | Qt.WindowDoesNotAcceptFocus,
        )
        self.dag = dag
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WA_NoSystemBackground, True)

        # Follow resizes / pan / zoom / parent window moves.
        dag.installEventFilter(self)
        if nuke_main is not None and nuke_main is not dag:
            nuke_main.installEventFilter(self)

        # Debounce repaints / geometry follow-ups.
        self._t = QTimer(self)
        self._t.setSingleShot(True)
        self._t.setInterval(16)  # ~60fps cap
        self._t.timeout.connect(self._follow_and_repaint)

        self._follow_and_repaint()

    # -- track DAG geometry in global screen coordinates --
    def _follow_and_repaint(self):
        try:
            if self.dag is None or not self.dag.isVisible():
                if self.isVisible():
                    self.hide()
                return
            tl = self.dag.mapToGlobal(QPoint(0, 0))
            w = self.dag.width()
            h = self.dag.height()
            if w <= 0 or h <= 0:
                if self.isVisible():
                    self.hide()
                return
            self.setGeometry(tl.x(), tl.y(), w, h)
            if not self.isVisible():
                self.show()
            self.raise_()  # keep above sibling child-windows of the Nuke main
            self.update()
        except Exception:
            pass

    def eventFilter(self, obj, event):
        t = event.type()
        if t in (
            QEvent.Resize,
            QEvent.Move,
            QEvent.Show,
            QEvent.Hide,
            QEvent.Paint,
            QEvent.Wheel,
            QEvent.MouseMove,
            QEvent.MouseButtonPress,
            QEvent.MouseButtonRelease,
            QEvent.WindowStateChange,
            QEvent.WindowActivate,
            QEvent.WindowDeactivate,
        ):
            self._t.start()
        return False

    # -- map a node's DAG-space rect to widget pixels --
    def _node_screen_rect(self, node):
        try:
            zoom = nuke.zoom()
            cx, cy = nuke.center()
        except Exception:
            return None
        w, h = self.width(), self.height()
        nx = node.xpos()
        ny = node.ypos()
        nw = node.screenWidth()
        nh = node.screenHeight()
        sx = (nx - cx) * zoom + w * 0.5
        sy = (ny - cy) * zoom + h * 0.5
        return QRectF(sx, sy, nw * zoom, nh * zoom)

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing, True)

        if DEBUG:
            # Magenta border + corner crosshairs so we can see where the
            # overlay window actually sits on screen, independent of whether
            # any nodes are being drawn.
            pen = p.pen()
            p.setPen(QColor(255, 0, 255, 220))
            p.setBrush(Qt.NoBrush)
            p.drawRect(0, 0, self.width() - 1, self.height() - 1)
            # Corner ticks.
            L = 20
            p.drawLine(0, 0, L, 0); p.drawLine(0, 0, 0, L)
            p.drawLine(self.width() - 1, 0, self.width() - 1 - L, 0)
            p.drawLine(self.width() - 1, 0, self.width() - 1, L)
            p.drawLine(0, self.height() - 1, L, self.height() - 1)
            p.drawLine(0, self.height() - 1, 0, self.height() - 1 - L)
            p.drawLine(self.width() - 1, self.height() - 1,
                       self.width() - 1 - L, self.height() - 1)
            p.drawLine(self.width() - 1, self.height() - 1,
                       self.width() - 1, self.height() - 1 - L)
            p.setPen(pen)

        try:
            zoom = nuke.zoom()
        except Exception:
            p.end()
            return
        if zoom < MIN_ZOOM:
            p.end()
            return

        try:
            nodes = nuke.allNodes(NODE_CLASS)
        except Exception:
            nodes = []
        if not nodes:
            p.end()
            return

        # Font scales gently with zoom, capped at sensible bounds.
        px = max(7, min(11, int(7 * zoom)))
        font = QFont("Helvetica", px)
        font.setBold(True)
        p.setFont(font)
        fm = QFontMetrics(font)

        for node in nodes:
            self._draw_badge(p, fm, node, zoom)

        p.end()

    def _draw_badge(self, p, fm, node, zoom):
        rect = self._node_screen_rect(node)
        if rect is None:
            return

        try:
            dev = int(node[DEVICE_KNOB].value())
        except Exception:
            return
        color = _PALETTE.get(dev, _PALETTE[2])
        label = _LABELS.get(dev, "?")

        # Pill sized to the label with a little horizontal padding.
        text_w = fm.horizontalAdvance(label)
        text_h = fm.height()
        pad_x = 6
        pad_y = 2
        pill_w = text_w + pad_x * 2
        pill_h = text_h + pad_y * 2

        # Anchor to the top-right corner of the node tile, overhanging slightly
        # outward so the badge reads as *attached to* rather than *on top of*.
        px_ = rect.right() - pill_w * 0.5
        py_ = rect.top()   - pill_h * 0.55
        pill = QRectF(px_, py_, pill_w, pill_h)
        radius = pill_h * 0.5

        # Drop shadow (cheap: offset translucent black rect beneath).
        p.setPen(Qt.NoPen)
        p.setBrush(_SHADOW)
        p.drawRoundedRect(pill.translated(0, 1), radius, radius)

        # Body.
        p.setBrush(color)
        p.drawRoundedRect(pill, radius, radius)

        # Label.
        p.setPen(_TEXT)
        p.drawText(pill, Qt.AlignCenter, label)


# ---------------------------------------------------------------------------
# Session-level management
# ---------------------------------------------------------------------------

_OVERLAYS = []  # strong refs so Python doesn't GC them


def install_spectral_badges(verbose=True):
    """
    Install the overlay on any DAG widgets that don't already have one.
    Safe to call repeatedly -- useful after new floating DAG panels open.
    """
    global _OVERLAYS
    # Drop overlays whose parent DAG has been destroyed.
    _OVERLAYS = [o for o in _OVERLAYS if o.parent() is not None]

    known = {o.parent() for o in _OVERLAYS}
    found = _find_dag_widgets(verbose=verbose)
    added = 0
    for dag in found:
        if dag not in known:
            _OVERLAYS.append(SpectralBadgeOverlay(dag))
            added += 1

    if verbose:
        nuke.tprint(
            "SpectralBadge: install -- found %d DAG widget(s), "
            "added %d overlay(s), total active %d"
            % (len(found), added, len(_OVERLAYS))
        )
    return len(_OVERLAYS)


def install_spectral_badges_with_retry(attempts=6, interval_ms=500):
    """
    Some Nuke startup paths create the DAG widget lazily (e.g. only after the
    first script loads). Retry until we find at least one DAG, or give up.
    """
    try:
        from PySide6.QtCore import QTimer
    except ImportError:
        from PySide2.QtCore import QTimer

    state = {"tries": 0}

    def _attempt():
        state["tries"] += 1
        n = install_spectral_badges(verbose=(state["tries"] == 1))
        if n == 0 and state["tries"] < attempts:
            QTimer.singleShot(interval_ms, _attempt)
        elif n == 0:
            nuke.tprint(
                "SpectralBadge: gave up after %d attempts -- run "
                "'from spectral_badge import diagnose; diagnose()' "
                "to inspect the widget tree." % attempts
            )

    _attempt()


def uninstall_spectral_badges():
    """Tear everything down. Useful during iterative development."""
    global _OVERLAYS
    for o in _OVERLAYS:
        try:
            o.setParent(None)
            o.deleteLater()
        except Exception:
            pass
    _OVERLAYS = []


def _poke_overlays():
    """Force a repaint on every overlay -- used by knob / create callbacks."""
    for o in _OVERLAYS:
        o._t.start()


def _on_device_knob_changed():
    # addKnobChanged fires for every knob on the node class; filter to ours.
    try:
        if nuke.thisKnob().name() == DEVICE_KNOB:
            _poke_overlays()
    except Exception:
        pass


def _register_callbacks():
    # Repaint when a SpectralRender is created or its device_mode toggles, so
    # the badge doesn't wait for the user to move the mouse over the DAG.
    nuke.addOnCreate(_poke_overlays, nodeClass=NODE_CLASS)
    nuke.addKnobChanged(_on_device_knob_changed, nodeClass=NODE_CLASS)


_register_callbacks()
