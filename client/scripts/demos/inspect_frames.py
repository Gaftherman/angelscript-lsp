#!/usr/bin/env python3
import os
import sys
import glob
from PIL import Image


def inspect(feature):
    frame_dir = os.path.join("_frames", feature)
    frames = sorted(glob.glob(os.path.join(frame_dir, "*.png")))
    if not frames:
        print(f"[{feature}] No frames found in {frame_dir}")
        return False

    print(f"[{feature}] Found {len(frames)} frames:")
    gif_path = os.path.join("client", "images", f"{feature}.gif")
    if os.path.exists(gif_path):
        size_kb = os.path.getsize(gif_path) / 1024
        print(f"  GIF size: {size_kb:.1f} KB (limit: 3072 KB)")

    all_ok = True
    for i, f in enumerate(frames):
        im = Image.open(f)
        w, h = im.size
        if w != 960 or h != 540:
            print(f"  Frame {i + 1} wrong size: {w}x{h}")
            all_ok = False

        # Check edge pixel samples (left 10px, right 10px, top 10px, bottom 10px)
        # Background is dark (~30, 30, 30)
        # Check if text or tooltip is clipped at the right edge (x >= 936)
        right_cutoff = False
        for y in range(40, h - 20):
            p = im.getpixel((w - 5, y))[:3]
            # If not dark background, something is touching right edge
            if max(abs(p[0] - 30), abs(p[1] - 30), abs(p[2] - 30)) > 35:
                # Could be a scrollbar or cutoff
                # Check if it's across multiple pixels
                right_cutoff = True
                break

        # Check top cutoff (y <= 24, excluding tabs/topbar if any)
        top_cutoff = False
        for x in range(100, w - 100):
            p = im.getpixel((x, 15))[:3]
            if max(abs(p[0] - 30), abs(p[1] - 30), abs(p[2] - 30)) > 40:
                top_cutoff = True
                break

        # Find content width
        content_xs = []
        for x in range(w):
            for y in range(40, h - 20):
                p = im.getpixel((x, y))[:3]
                if max(abs(p[0] - 30), abs(p[1] - 30), abs(p[2] - 30)) > 30:
                    content_xs.append(x)
                    break
        min_x = min(content_xs) if content_xs else 0
        max_x = max(content_xs) if content_xs else w
        span = max_x - min_x

        status = []
        if right_cutoff:
            status.append("RIGHT_EDGE_TOUCH")
        if top_cutoff:
            status.append("TOP_EDGE_TOUCH")
        if max_x < int(w * 0.75):
            status.append(f"EMPTY_RIGHT_STRIP(max_x={max_x}/{w})")

        status_str = ", ".join(status) if status else "OK"
        print(f"  Frame {i + 1} ({os.path.basename(f)}): content [{min_x}..{max_x}] span={span}px -> {status_str}")

    return all_ok


if __name__ == "__main__":
    feats = sys.argv[1:] if len(sys.argv) > 1 else ["hover"]
    for feat in feats:
        inspect(feat)
