# make_examples.py - builds the example compositions in this folder (examples/*.ntp).
#
# The .ntp files are in the repository already; run this only to rebuild them after
# changing this script. It must run in the Natron GUI (the headless NatronRenderer does
# not save node positions, so the graphs would open as one pile of nodes):
#     cd ~/projects-own/natron-kdenlive-link/examples
#     NKB_EXAMPLES_DIR="$PWD" snap run natron -c "$(cat make_examples.py)"
# Natron opens, writes the files, and quits.
#
# Every graph follows the bridge's rules (see README, "Using Natron compositions"):
#   * the clip's frame comes in through the Read node NKB_Input,
#   * the result leaves through the Write node NKB_Output,
#   * the result has the size of the clip (Merge nodes keep the clip's bounding box, "B").
# Positions and sizes are in pixels of a 1920x1080 clip, origin at the bottom left (Natron's
# convention). Animations use expressions of "frame", the frame number counted from the
# start of the Kdenlive effect, so they run for as long as the clip does.
import glob
import os
import re
import sys
import traceback

OUT_DIR = os.environ.get("NKB_EXAMPLES_DIR") or os.getcwd()
W, H = 1920, 1080


def say(text):
    sys.__stdout__.write("NKB_EXAMPLES " + text + "\n")
    sys.__stdout__.flush()


def node(app, plugin, label, x, y):
    n = app.createNode(plugin)
    n.setLabel(label)
    n.setPosition(x, y)
    return n


def begin(app):
    """Fresh project with the two nodes every composition needs."""
    app.resetProject()
    r = node(app, "fr.inria.built-in.Read", "NKB_Input", 0, 0)
    r.setScriptName("NKB_Input")
    w = node(app, "fr.inria.built-in.Write", "NKB_Output", 0, 600)
    w.setScriptName("NKB_Output")
    return r, w


def save(app, name, w, last):
    w.connectInput(0, last)
    path = os.path.join(OUT_DIR, name + ".ntp")
    app.saveProject(path)
    neutral_paths(path)
    say("saved " + path)


def neutral_paths(path):
    """Natron stores its own install folder and the project folder in the project settings ("Project
    Paths"). Both are reset when a project is loaded; the stored values would only show where the file
    was made, so they are replaced by those of the Natron snap and a folder in the home."""
    text = open(path).read()
    text = re.sub(r"&lt;Name&gt;OCIO&lt;/Name&gt;&lt;Value&gt;.*?&lt;/Value&gt;&lt;Name&gt;Project&lt;/Name&gt;&lt;Value&gt;.*?&lt;/Value&gt;",
                  "&lt;Name&gt;OCIO&lt;/Name&gt;&lt;Value&gt;/snap/natron/current/Resources/OpenColorIO-Configs/blender&lt;/Value&gt;"
                  "&lt;Name&gt;Project&lt;/Name&gt;&lt;Value&gt;~/NatronKdenliveLink/examples&lt;/Value&gt;", text)
    open(path, "w").write(text)


def merge_over(app, label, b, a, x, y):
    """A over B; the output keeps B's bounding box, i.e. the clip's size."""
    m = node(app, "net.sf.openfx.MergePlugin", label, x, y)
    m.connectInput(0, b)  # input 0 is B (background)
    m.connectInput(1, a)  # input 1 is A (foreground)
    m.getParam("bbox").set(3)  # B
    return m


def expr(param, text, dim=0):
    param.setExpression(text, False, dim)


def invert(app):
    """Negative image. The alpha channel is left alone: inverting it would make the clip transparent."""
    r, w = begin(app)
    inv = node(app, "net.sf.openfx.Invert", "Invert colours", 0, 300)
    inv.connectInput(0, r)
    inv.getParam("NatronOfxParamProcessA").setValue(False)
    save(app, "invert", w, inv)


def bouncing_ball(app):
    """An orange ball bounces across the picture; the clip shows everywhere else."""
    r, w = begin(app)
    d = 220  # ball diameter in pixels
    ball = node(app, "net.sf.openfx.Radial", "Ball", 300, 150)
    ball.getParam("size").set(d, d)
    ball.getParam("softness").setValue(0.08)  # a crisp edge, slightly smoothed
    ball.getParam("color0").set(0, 0, 0, 0)  # outside the ball: transparent
    ball.getParam("color1").set(1.0, 0.45, 0.05, 1.0)  # inside: orange, opaque
    bl = ball.getParam("bottomLeft")
    # x goes back and forth across the frame, 24 px per frame; y bounces like a ball on the floor.
    expr(bl, "abs((frame*24) %% %d - %d)" % (2 * (W - d), W - d), 0)
    expr(bl, "30 + %d*abs(sin(frame*pi/30))" % (H - d - 80), 1)
    m = merge_over(app, "Ball over clip", r, ball, 0, 300)
    save(app, "bouncing_ball", w, m)


def spotlight(app):
    """The clip turns black and white except inside a soft circle that circles around the picture."""
    r, w = begin(app)
    d = 520
    mask = node(app, "net.sf.openfx.Radial", "Spotlight mask", 300, 150)
    mask.getParam("size").set(d, d)
    mask.getParam("softness").setValue(0.35)
    bl = mask.getParam("bottomLeft")
    expr(bl, "%d + 560*cos(frame*0.05)" % (W / 2 - d / 2), 0)
    expr(bl, "%d + 260*sin(frame*0.05)" % (H / 2 - d / 2), 1)
    bw = node(app, "net.sf.openfx.SaturationPlugin", "Black and white outside", 0, 300)
    bw.connectInput(0, r)
    bw.connectInput(1, mask)  # input 1 is the mask
    bw.getParam("saturation").setValue(0.0)
    bw.getParam("NatronOfxParamProcessA").setValue(False)
    bw.getParam("enableMask_Mask").setValue(True)
    bw.getParam("maskInvert").setValue(True)  # the effect applies outside the circle
    save(app, "spotlight", w, bw)


def vignette(app):
    """Darkens the corners of the picture."""
    r, w = begin(app)
    rw, rh = 2400, 1700  # an ellipse a bit larger than the frame
    v = node(app, "net.sf.openfx.Radial", "Dark edges", 300, 150)
    v.getParam("bottomLeft").set((W - rw) / 2, (H - rh) / 2)
    v.getParam("size").set(rw, rh)
    v.getParam("softness").setValue(1.0)
    v.getParam("color0").set(0, 0, 0, 0.8)  # edges: black, 80 % opaque (premultiplied)
    v.getParam("color1").set(0, 0, 0, 0)  # centre: untouched
    m = merge_over(app, "Vignette over clip", r, v, 0, 300)
    save(app, "vignette", w, m)


def title(app):
    """A "lower third" title: a dark band with text at the bottom of the picture, fading in during the
    first second. Change the words in the Title text node (its Text field)."""
    r, w = begin(app)
    band = node(app, "net.sf.openfx.Rectangle", "Dark band", 600, 0)
    band.getParam("bottomLeft").set(0, 50)
    band.getParam("size").set(W, 190)
    band.getParam("color0").set(0, 0, 0, 0)  # outside the band: transparent
    band.getParam("color1").set(0, 0, 0, 0.6)  # the band: black, 60 % opaque (premultiplied)
    text = node(app, "net.fxarena.openfx.Text", "Title text", 300, 0)
    text.getParam("text").setValue("Natron + Kdenlive")
    text.getParam("size").setValue(110)
    text.getParam("color").set(1, 1, 1, 1)
    # "center" is where the text starts (its upper left corner, roughly; Natron 2.5.0 ignores the
    # horizontal alignment setting), so the words grow to the right from x = 120.
    text.getParam("center").set(106, 225)
    words = merge_over(app, "Text on band", band, text, 450, 150)
    m = merge_over(app, "Fade in title", r, words, 0, 300)
    # Two keyframes (they can be moved in Natron's Curve Editor): fully transparent at the effect's
    # start, fully visible from frame 25 on.
    mix = m.getParam("mix")
    mix.setValueAtTime(0.0, 0)
    mix.setValueAtTime(1.0, 25)
    save(app, "title", w, m)


def spinning_picture(app):
    """The clip, at half size, spins slowly over a blurred and darkened copy of itself."""
    r, w = begin(app)
    blur = node(app, "net.sf.cimg.CImgBlur", "Blur background", -300, 150)
    blur.connectInput(0, r)
    blur.getParam("size").set(60, 60)
    blur.getParam("boundary").set(1)  # "nearest": no dark or light seam at the picture's edges
    blur.getParam("expandRoD").setValue(False)  # keep the clip's size
    dark = node(app, "net.sf.openfx.GradePlugin", "Darken background", -300, 300)
    dark.connectInput(0, blur)
    dark.getParam("multiply").set(0.5, 0.5, 0.5, 1.0)
    spin = node(app, "net.sf.openfx.TransformPlugin", "Half size, spinning", 300, 150)
    spin.connectInput(0, r)
    spin.getParam("center").set(W / 2, H / 2)
    spin.getParam("scale").set(0.5, 0.5)
    expr(spin.getParam("rotate"), "frame*2")  # degrees: one turn in 180 frames
    m = merge_over(app, "Picture over background", dark, spin, 0, 450)
    save(app, "spinning_picture", w, m)


try:
    os.makedirs(OUT_DIR, exist_ok=True)
    for make in (invert, bouncing_ball, spotlight, vignette, title, spinning_picture):
        make(app1)
    app1.resetProject()
    for lock in glob.glob(os.path.join(OUT_DIR, "*.ntp.lock")):
        os.remove(lock)
    say("done")
except Exception:
    say("error " + traceback.format_exc())
sys.__stdout__.flush()
os._exit(0)
