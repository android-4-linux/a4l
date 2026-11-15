#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l tooling. No GPL-3.0 sources.
"""Capture a screen region and a PipeWire sink's monitor on one clock, and report when the
picture flashes and when the sound beeps.

  measure-av-sync.py <wayland display> <sink name> <seconds> <x,y> [label]

The clip under test flashes white and beeps at the same instants. Each flash onset is paired
with the nearest beep onset; the difference is audio minus video, in ms (positive: the sound
is late).

Clock: CLOCK_MONOTONIC in this process for both.
  video  a small region is grabbed with grim in a loop (about 60 a second on a headless
         sway). An onset lies between the last dark sample and the first bright one; the
         midpoint is taken, so it is known to half the sampling gap, and the grab itself
         returns a frame the compositor has already drawn.
  audio  pw-record streams raw samples to a pipe. A sample's time is its index over the rate
         plus the smallest (arrival time - samples so far / rate) seen, which removes the
         scheduling delay of each read and leaves the capture's own constant latency.
Both paths have a constant delay this cannot see. It is taken out by running a reference
player through the same capture and subtracting. How to set up the compositor, the sink and
the clip, and the reference run, is in tests/fixtures/flutter_video_player/README.md.

Needs grim, pw-record and numpy, and a compositor grim can read (headless sway).
"""
import os, subprocess, sys, threading, time
import numpy as np

display, sink, seconds, region = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
label = sys.argv[5] if len(sys.argv) > 5 else ""
RATE = 48000
env = dict(os.environ, WAYLAND_DISPLAY=display)
stop = time.monotonic() + seconds

video = []   # (t0, t1, mean brightness)
def grab():
    x, y = region.split(",")
    while time.monotonic() < stop:
        t0 = time.monotonic()
        r = subprocess.run(["grim", "-g", f"{x},{y} 8x8", "-t", "ppm", "-"], env=env,
                           capture_output=True)
        t1 = time.monotonic()
        if r.returncode == 0 and len(r.stdout) > 192:
            video.append((t0, t1, float(np.frombuffer(r.stdout[-192:], dtype=np.uint8).mean())))

chunks = []  # (arrival, samples so far)
pcm = []
def listen():
    p = subprocess.Popen(["pw-record", "--raw", "--target", sink, "-P", "stream.capture.sink=true",
                          "--rate", str(RATE), "--channels", "1", "--format", "s16",
                          "--latency", "256/48000", "-"], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, env={k: v for k, v in os.environ.items()
                                                         if k != "PIPEWIRE_NODE"})
    total = 0
    fd = p.stdout.fileno()
    while time.monotonic() < stop:
        b = os.read(fd, 65536)
        if not b:
            break
        t = time.monotonic()
        b = b[:len(b) // 2 * 2]
        pcm.append(np.frombuffer(b, dtype="<i2"))
        total += len(b) // 2
        chunks.append((t, total))
    p.terminate()

tv, ta = threading.Thread(target=grab), threading.Thread(target=listen)
tv.start(); ta.start(); tv.join(); ta.join()

# Video onsets.
von = []
for i in range(1, len(video)):
    if video[i][2] > 128 and video[i - 1][2] <= 128:
        von.append(((video[i - 1][1] + video[i][1]) / 2, video[i][1] - video[i - 1][1]))
gaps = [video[i][0] - video[i - 1][0] for i in range(1, len(video))]
# Audio onsets.
aon = []
if pcm:
    a = np.abs(np.concatenate(pcm).astype(float))
    base = min(t - n / RATE for t, n in chunks)
    idx = np.nonzero(a > 1500)[0]
    if len(idx):
        starts = [idx[0]] + [idx[i] for i in range(1, len(idx)) if idx[i] - idx[i - 1] > RATE // 5]
        aon = [base + s / RATE for s in starts]
print(f"{label}: {len(video)} screen samples (median gap {np.median(gaps)*1000:.1f} ms), "
      f"{sum(len(p) for p in pcm)/RATE:.2f} s of audio, {len(von)} flashes, {len(aon)} beeps")
if von:
    t0 = von[0][0]
    print(f"{label}: flashes at s: " + " ".join(f"{t - t0:.3f}" for t, _ in von))
    print(f"{label}: beeps at s:   " + " ".join(f"{t - t0:.3f}" for t in aon))
    # How long each flash stayed on screen.
    on, runs = None, []
    for t0_, t1_, b in video:
        if b > 128 and on is None:
            on = t1_
        elif b <= 128 and on is not None:
            runs.append(t1_ - on); on = None
    print(f"{label}: flash lengths ms: " + " ".join(f"{r*1000:.0f}" for r in runs))
diffs = []
for t, width in von:
    if not aon:
        break
    nearest = min(aon, key=lambda x: abs(x - t))
    if abs(nearest - t) < 0.4:
        diffs.append((nearest - t) * 1000)
if diffs:
    d = np.array(diffs)
    print(f"{label}: audio - video per event, ms: " + " ".join(f"{x:+.0f}" for x in d))
    print(f"{label}: mean {d.mean():+.1f} ms, median {np.median(d):+.1f}, min {d.min():+.1f}, "
          f"max {d.max():+.1f}, n={len(d)}")
    if len(von) > 1:
        iv = np.diff([t for t, _ in von]) * 1000
        print(f"{label}: flash-to-flash {iv.mean():.1f} ms (clip: 1000)")
