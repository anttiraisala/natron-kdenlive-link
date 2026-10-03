# nkb_gui_open.py - prepares a composition in the Natron GUI for "Open in Natron".
#
# The daemon starts the Natron GUI with this file's text as a -c command (so the snap
# does not need to read the file) and the composition in NKB_OPEN:
#     NKB_OPEN=/path/comp-xxxxxx.ntp  Natron -c "<text of this file>"
#
# Natron has no option to run a script after it loaded a project given on the command
# line, so this script loads the project itself. Then:
#   * a composition that does not exist yet is created: Read node NKB_Input -> Write node
#     NKB_Output, the same pass-through graph the worker creates, and saved;
#   * NKB_Input reads <name>_preview.tga next to the .ntp when it exists: the frame the
#     Kdenlive effect saved when "Open in Natron" was clicked;
#   * NKB_Output gets the file name <name>_output.tga if it has none (otherwise Natron
#     shows the node in error and the viewer stays black; the worker sets its own file
#     names for every frame and never saves the project, so neither name matters there);
#   * a viewer is connected to NKB_Output.
# Changing the file names marks the project as modified; Natron asks before closing.
# Paths are set absolute on purpose: Natron itself turns a path inside the project folder
# into "[Project]/..."; passing "[Project]/..." makes it add the prefix again (2.5.0).
# Messages start with "NKB_GUI" and end up in <data dir>/logs/natron-gui.log.
import os
import sys
import traceback


def _nkb_say(text):
    # The GUI catches sys.stdout for its Script Editor; the original stream goes to the log.
    sys.__stdout__.write("NKB_GUI " + text + "\n")
    sys.__stdout__.flush()


def _nkb_gui_open():
    path = os.environ.get("NKB_OPEN", "")
    if not path.endswith(".ntp"):
        _nkb_say("error NKB_OPEN must name a .ntp file, got %r" % path)
        return
    stem = os.path.splitext(path)[0]
    if os.path.exists(path):
        app = app1.loadProject(path) or app1
        _nkb_say("loaded %s" % path)
    else:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        app = app1
        r = app.createNode("fr.inria.built-in.Read")
        r.setScriptName("NKB_Input")
        w = app.createNode("fr.inria.built-in.Write")
        w.setScriptName("NKB_Output")
        w.connectInput(0, r)
        r.setPosition(0, 0)
        w.setPosition(0, 200)
        w.getParam("filename").setValue(stem + "_output.tga")
        app.saveProject(path)
        _nkb_say("created %s" % path)
    r = app.getNode("NKB_Input")
    w = app.getNode("NKB_Output")
    if r is None or w is None:
        _nkb_say("error %s has no nodes named NKB_Input and NKB_Output" % path)
        return
    preview = stem + "_preview.tga"
    if os.path.exists(preview):
        r.getParam("filename").setValue(preview)
        # The frame is 8 bit sRGB, as the worker treats its input (NKB_COLOR=srgb). Natron picks the colourspace from
        # the file type whenever a file name changes, so it is set after the name, like the worker does.
        try:
            r.getParam("filePremult").set(2)  # straight (un-premultiplied) alpha
            r.getParam("ocioInputSpace").set("sRGB")
        except Exception:
            _nkb_say("could not set the colourspace of NKB_Input")
        _nkb_say("preview %s" % preview)
    if not w.getParam("filename").getValue():
        w.getParam("filename").setValue(stem + "_output.tga")
    # This runs before the GUI has created its own viewer, so connect the viewer(s) a moment later.
    def connect_viewers(attempt=0):
        try:
            viewers = [n for n in app.getChildren() if n.getPluginID() == "fr.inria.built-in.Viewer"]
            if not viewers and attempt < 20:
                QtCore.QTimer.singleShot(250, lambda: connect_viewers(attempt + 1))
                return
            for v in viewers:
                v.connectInput(0, w)
            _nkb_say("viewer connected to NKB_Output (%d viewer(s), attempt %d)" % (len(viewers), attempt))
        except Exception:
            _nkb_say("error " + traceback.format_exc())
    QtCore = None
    for name in ("PySide", "PySide2", "qtpy"):  # Natron 2.5 ships PySide (1.2) and qtpy
        try:
            QtCore = __import__(name, fromlist=["QtCore"]).QtCore
            break
        except Exception:
            pass
    if QtCore is None:
        _nkb_say("no Qt for Python found: connect a viewer to NKB_Output by hand")
        return
    QtCore.QTimer.singleShot(500, connect_viewers)


try:
    _nkb_gui_open()
except Exception:
    _nkb_say("error " + traceback.format_exc())
