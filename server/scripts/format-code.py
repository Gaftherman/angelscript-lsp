#!/usr/bin/env python3
"""Applies or verifies clang-format across all AngelLSP C++ sources and headers.

Usage:
    python server/scripts/format-code.py         # Format in-place
    python server/scripts/format-code.py --check # Verification only (exit 1 if unformatted)
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
TARGET_DIRS = [
    REPO_ROOT / 'server' / 'src',
    REPO_ROOT / 'server' / 'include',
    REPO_ROOT / 'server' / 'tests',
]
EXTENSIONS = {'.cpp', '.h', '.hpp'}

def find_files() -> list[Path]:
    files = []
    for d in TARGET_DIRS:
        if not d.exists():
            continue
        for root, _, filenames in os.walk(d):
            for f in filenames:
                p = Path(root) / f
                if p.suffix in EXTENSIONS:
                    files.append(p)
    return sorted(files)

def main() -> int:
    parser = argparse.ArgumentParser(description="Format AngelLSP C++ code with clang-format.")
    parser.add_argument("--check", action="store_true", help="Check formatting without modifying files.")
    args = parser.parse_args()

    clang_format = shutil.which("clang-format")
    if not clang_format:
        print("[WARNING] clang-format executable not found in PATH.", file=sys.stderr)
        return 0 if not args.check else 1

    files = find_files()
    if not files:
        print("[INFO] No C++ files found to format.")
        return 0

    if args.check:
        print(f"[CHECKING] Checking clang-format style on {len(files)} files...")
        unformatted = []
        for file in files:
            cmd = [clang_format, "--dry-run", "--Werror", str(file)]
            res = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO_ROOT)
            if res.returncode != 0:
                unformatted.append(file)
        if unformatted:
            print(f"[FAILED] {len(unformatted)} file(s) require formatting:", file=sys.stderr)
            for f in unformatted:
                print(f"  - {f.relative_to(REPO_ROOT)}", file=sys.stderr)
            print("\nRun 'python server/scripts/format-code.py' to format automatically.", file=sys.stderr)
            return 1
        print(f"[PASSED] All {len(files)} files adhere to .clang-format.")
        return 0
    else:
        print(f"[FORMATTING] In-place formatting {len(files)} C++ files...")
        batch_size = 50
        for i in range(0, len(files), batch_size):
            batch = [str(f) for f in files[i:i + batch_size]]
            cmd = [clang_format, "-i"] + batch
            res = subprocess.run(cmd, cwd=REPO_ROOT)
            if res.returncode != 0:
                print(f"[ERROR] clang-format failed with exit code {res.returncode}", file=sys.stderr)
                return res.returncode
        print(f"[DONE] Formatted {len(files)} files successfully.")
        return 0

if __name__ == "__main__":
    sys.exit(main())
