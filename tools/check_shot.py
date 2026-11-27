#!/usr/bin/env python3
"""Sanity-check viewer screenshots: region stats + expected color presence."""
import sys
from PIL import Image
import numpy as np

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/shot1.png"
im = np.asarray(Image.open(path).convert("RGB")).astype(int)
H, W, _ = im.shape
print(f"{path}: {W}x{H}")

def region(name, y0, y1, x0=0, x1=None):
    x1 = x1 or W
    r = im[y0:y1, x0:x1]
    mean = r.reshape(-1, 3).mean(axis=0)
    # count "non-background" pixels (brighter than panel color)
    bright = (r.sum(axis=2) > 90).mean()
    print(f"  {name:24s} y{y0}-{y1} mean=({mean[0]:5.1f},{mean[1]:5.1f},{mean[2]:5.1f}) content={bright*100:5.1f}%")
    return r

# layout mirrors viewer: topbar 52, timeline 170, table 176, inspector 430
topbar = region("topbar", 0, 52)
timeline = region("timeline", 52, 222)
graph = region("graph", 222, H-176, 0, W-430)
inspector = region("inspector", 222, H-176, W-430, W)
table = region("table", H-176, H)

def has(rgb, tol=30, where=None):
    r = im if where is None else where
    m = (np.abs(r - np.array(rgb)) < tol).all(axis=2)
    print(f"  color {rgb}: {m.sum()} px", "FOUND" if m.sum() > 5 else "*** MISSING ***")
    return m.sum()

# playhead yellow (255,216,102)
print("expect playhead yellow in timeline+graph:")
has((255, 216, 102), where=np.concatenate([timeline, graph]))
# node colors: mine amber 227,179,65 ; flot green 63,185,80 ; acid cyan 57,197,207
print("expect node accents:")
has((227, 179, 65), where=graph)
has((63, 185, 80), where=graph)
has((57, 197, 207), where=graph)
# text-ish pixels (light gray ~201)
print("text presence per region:")
for name, r in [("topbar", topbar), ("timeline", timeline), ("graph", graph), ("inspector", inspector), ("table", table)]:
    m = (np.abs(r - np.array([201, 209, 217])) < 40).all(axis=2)
    print(f"  {name:10s} text pixels: {m.sum()}")
# mark lane colors in timeline bottom: amber 219,171,40 / bad red 248,81,73
print("expect mark lanes in timeline:")
has((219, 171, 40), where=timeline)
has((248, 81, 73), where=timeline)
