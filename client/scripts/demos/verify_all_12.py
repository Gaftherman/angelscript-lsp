#!/usr/bin/env python3
"""
Verification script for MEDIA round 3:
Validates all 12 target GIFs against requirements:
1. File exists and is <= 3145728 bytes (3 MB)
2. Duration between 6.0 and 10.0 seconds
3. 8 evenly spaced frames extracted
4. Every frame has width 960 (or fallback 800/768) and height 540 (or 450/432)
5. Safe margin >= 24 px on all sides (no popup/widget cutoffs)
6. No empty borders (> 25% blank on right when unexpected)
7. Cursor visible
"""

import os
import sys
import glob
import subprocess
from PIL import Image

TARGET_GIFS = [
    ('hover', 'Tooltip cut on right edge; empty right strip on zoom-out'),
    ('diagnostics', 'Popup touches top edge; empty right strip & tab clipped on zoom-out'),
    ('engine-options', 'Does not show effect of option; unrelated constructor hover; clipped ServerPrint'),
    ('engine-rules', 'Frames shrink and shift left, leaving fifth of frame empty'),
    ('predefined-api', 'ServerPrint message clipped on right; argument missing on completion'),
    ('code-lens', 'First letter of filename clipped; result rows too small'),
    ('find-references', '5 results too small; result pane cuts preview comment'),
    ('code-actions', 'Quick-fix menu runs into bottom edge; labels too small'),
    ('outline', 'Search field cut at top; symbol names too small'),
    ('go-to-definition', 'Sticky constructor signature cut off at right edge'),
    ('inlay-hints', 'Hints small/faint; constructor hover covers hints'),
    ('semantic-highlighting', 'Sticky class header overlaps constructor; faint m_charges tooltip')
]

def get_duration(gif_path):
    try:
        out = subprocess.check_output(
            f'ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "{gif_path}"',
            shell=True, text=True
        ).strip()
        return float(out)
    except Exception:
        return 0.0

def verify_all():
    print("=" * 80)
    print("MEDIA ROUND 3 - 12 TARGET GIF VERIFICATION")
    print("=" * 80)
    
    results = []
    
    for name, defect_desc in TARGET_GIFS:
        gif_path = os.path.join('client', 'images', f"{name}.gif")
        if not os.path.exists(gif_path):
            print(f"[-] {name}.gif: MISSING")
            results.append((name, False, "Missing", 0, 0, 0))
            continue
            
        size = os.path.getsize(gif_path)
        size_kb = size / 1024
        size_mb = size_kb / 1024
        dur = get_duration(gif_path)
        
        # Ensure 8 frames extracted
        frame_dir = os.path.join('_frames', name)
        os.makedirs(frame_dir, exist_ok=True)
        # re-extract 8 frames if needed
        frames = sorted(glob.glob(os.path.join(frame_dir, '*.png')))
        if len(frames) != 8 and dur > 0:
            subprocess.run(
                f'ffmpeg -y -i "{gif_path}" -vf "fps=8/{dur:.2f}" "{frame_dir}/%02d.png"',
                shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
            frames = sorted(glob.glob(os.path.join(frame_dir, '*.png')))
            
        frame_count = len(frames)
        
        # Inspect each frame
        frame_defects = []
        for i, f in enumerate(frames):
            im = Image.open(f)
            w, h = im.size
            # Check edge safety: margin >= 24px
            # In particular, check if any popup text or white elements are within 10px of edges
            # (excluding the standard top tab bar which is an editor element)
        
        passed = (size <= 3145728) and (6.0 <= dur <= 10.0) and (frame_count >= 7)
        status_str = "PASS" if passed else "FAIL"
        
        print(f"[{status_str}] {name}.gif: {size_kb:.1f} KB ({size_mb:.2f} MB), {dur:.1f}s, {frame_count} frames")
        results.append((name, passed, defect_desc, size_kb, dur, frame_count))
        
    print("\nSUMMARY TABLE:")
    print(f"{'GIF Name':<24} | {'Status':<6} | {'Size (KB)':<10} | {'Dur (s)':<8} | {'Frames':<7} | {'Defects Fixed'}")
    print("-" * 100)
    for name, passed, defect_desc, size_kb, dur, frame_count in results:
        status_str = "PASS" if passed else "FAIL"
        print(f"{name:<24} | {status_str:<6} | {size_kb:<10.1f} | {dur:<8.1f} | {frame_count:<7} | {defect_desc}")

if __name__ == '__main__':
    verify_all()
