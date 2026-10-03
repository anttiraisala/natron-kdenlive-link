# make_test_comp.py - run inside Natron to create test compositions for the worker tests:
#     NatronRenderer -t make_test_comp.py
# Writes <NKB_COMPS_DIR>/invert_rgb.ntp : NKB_Input -> Invert (RGB only, alpha untouched) -> NKB_Output
import os
import sys

comps = os.environ["NKB_COMPS_DIR"]
os.makedirs(comps, exist_ok=True)
r = app1.createNode("fr.inria.built-in.Read")
r.setScriptName("NKB_Input")
inv = app1.createNode("net.sf.openfx.Invert")
inv.getParam("NatronOfxParamProcessA").setValue(False)
w = app1.createNode("fr.inria.built-in.Write")
w.setScriptName("NKB_Output")
inv.connectInput(0, r)
w.connectInput(0, inv)
app1.saveProject(os.path.join(comps, "invert_rgb.ntp"))
print("made invert_rgb.ntp")
sys.stdout.flush()
os._exit(0)
