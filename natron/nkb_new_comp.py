# nkb_new_comp.py - creates a new pass-through composition for natron-kdenlive-link.
#
# Run headless inside Natron by the daemon before "Open in Natron" opens a file that
# does not exist yet:
#     NKB_NEW_COMP=/path/comp-xxxxxx.ntp  NatronRenderer -t nkb_new_comp.py
#     NKB_NEW_COMP=/path/comp-xxxxxx.ntp  snap run natron -t nkb_new_comp.py
#
# The graph is the same default the worker creates (nkb_natron_worker.py,
# CompManager._create_default): a Read node named NKB_Input connected to a Write node
# named NKB_Output. Keep the two in step.
#
# Prints "NKB_NEW_COMP ok <path>" or "NKB_NEW_COMP error <reason>" and exits with 0 or 1.
# Exits through os._exit because Natron 2.5.0 can crash while its interpreter shuts down.
import os
import sys


def main():
    path = os.environ.get("NKB_NEW_COMP", "")
    if not path.endswith(".ntp"):
        print("NKB_NEW_COMP error NKB_NEW_COMP must name a .ntp file, got %r" % path)
        return 1
    if os.path.exists(path):
        print("NKB_NEW_COMP ok %s (already exists, not changed)" % path)
        return 0
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    app1.resetProject()
    r = app1.createNode("fr.inria.built-in.Read")
    r.setScriptName("NKB_Input")
    w = app1.createNode("fr.inria.built-in.Write")
    w.setScriptName("NKB_Output")
    w.connectInput(0, r)
    app1.saveProject(path)
    if not os.path.exists(path):
        print("NKB_NEW_COMP error Natron did not write %s" % path)
        return 1
    print("NKB_NEW_COMP ok %s" % path)
    return 0


try:
    rc = main()
except Exception as e:  # report instead of leaving Natron hanging
    print("NKB_NEW_COMP error %s" % e)
    rc = 1
sys.stdout.flush()
os._exit(rc)
