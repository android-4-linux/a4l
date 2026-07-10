# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), Phase 2 gfx/Canvas. No GPL-3.0 sources.
#
# Prove the Phase-2 rendering vertical: gfx.apk's main() constructs a custom
# com.aclcxx.hello.MyView extends android.view.View, a Bitmap-backed Canvas, and
# calls v.draw(c). View.draw() dispatches to the overridden MyView.onDraw(Canvas),
# which fills blue (drawColor 0xFF0000FF) then draws a red rect (drawRect 10,10,50,50
# with a Paint set to 0xFFFF0000). The Canvas/Bitmap natives raster into an
# aclcxx::gfx::Surface; aclcxxDescribe() reads pixels back. This exercises a custom
# View.onDraw override + invoke-virtual/range + float args through the DEX->JVM
# translator, and verifies the resulting pixels with no compositor.
#
# Usage: run_gfx_test.py <aclcxx_binary> <gfx.apk> <framework_dir>
import subprocess
import sys

# 64x64 bitmap: (0,0) is the blue background; (32,32) is inside the 10..50 red rect.
EXPECT = "bitmap 64x64 pixel(0,0)=0xff0000ff pixel(32,32)=0xffff0000"


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_gfx_test.py <aclcxx> <gfx.apk> <fw_dir>\n")
        return 2
    aclcxx, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    p = subprocess.run([aclcxx, "run", apk, "--vm=jvm", "--framework=" + fw],
                       capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = True
    if p.returncode != 0:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n"); ok = False
    if EXPECT not in out:
        sys.stderr.write(f"FAIL: expected pixel summary not found:\n  {EXPECT}\n"); ok = False
    if "unsupported" in out.lower() or "Exception" in out:
        sys.stderr.write("FAIL: translation/runtime error in gfx path\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: MyView.onDraw -> Canvas -> gfx rastered verifiable pixels\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
