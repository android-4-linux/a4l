# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), Phase 2 View traversal. No GPL-3.0 sources.
#
# Prove the Phase-2 View(Context) + measure/layout/draw traversal over a real view
# tree. view.apk's launcher MainActivity (an Activity) does, in onCreate:
#   setContentView(new Panel(this));
# where Panel extends android.view.ViewGroup and holds two Tiles (custom Views, each
# a color). After driving onCreate->onStart->onResume, the acl ViewRoot driver
# (jvm_vm render_content_view, standing in for ViewRootImpl.performTraversals):
#   measure(EXACTLY W, EXACTLY H) -> layout(0,0,mw,mh) -> draw(canvas)
# Panel.onLayout stacks the tiles vertically (top half / bottom half); each Tile.onDraw
# fills its laid-out box, translated into place by ViewGroup.dispatchDraw. On the
# 1920x1080 display the top half rasters blue and the bottom half red. This exercises
# the whole traversal, invoke-virtual/range + int-to-float + instance fields through
# the DEX->JVM translator, and Canvas save/translate/restore — verified via pixels.
#
# Usage: run_view_test.py <acl_binary> <view.apk> <framework_dir>
import subprocess
import sys

LAUNCHING = "launching Activity com.acl.hello.MainActivity"
TRAVERSAL = "ViewRoot: performTraversals measured=1920x1080"
# 1920x1080: (0,0) in the top (blue) tile; (960,540) is the top of the bottom (red) tile.
PIXELS = "bitmap 1920x1080 pixel(0,0)=0xff0000ff pixel(960,540)=0xffff0000"


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_view_test.py <acl> <view.apk> <fw_dir>\n")
        return 2
    acl, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    p = subprocess.run([acl, "run", apk, "--vm=jvm", "--framework=" + fw],
                       capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = True
    if p.returncode != 0:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n"); ok = False
    if LAUNCHING not in out:
        sys.stderr.write("FAIL: did not take the Activity-lifecycle path\n"); ok = False
    if TRAVERSAL not in out:
        sys.stderr.write("FAIL: ViewRoot measure/layout traversal did not run\n"); ok = False
    if PIXELS not in out:
        sys.stderr.write(f"FAIL: expected pixel summary not found:\n  {PIXELS}\n"); ok = False
    if "stubbed" in out and "0 stubbed" not in out:
        sys.stderr.write("FAIL: a view method fell back to a stub\n"); ok = False
    if "Exception" in out:
        sys.stderr.write("FAIL: runtime exception in the traversal\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: measure/layout/draw over the Panel/Tile tree rastered verifiable pixels\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
