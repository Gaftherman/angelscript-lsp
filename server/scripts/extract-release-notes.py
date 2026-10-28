#!/usr/bin/env python3
"""
Extracts the markdown release notes section for a specific tag/version from CHANGELOG.md.

Usage:
    python server/scripts/extract-release-notes.py --tag v1.1.6-exp.2 --output release-notes.md
"""

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def extract_notes(changelog_path: Path, tag: str) -> tuple[str, str] | None:
    if not changelog_path.exists():
        print(f"Error: Changelog file '{changelog_path}' does not exist.", file=sys.stderr)
        return None

    with open(changelog_path, "r", encoding="utf-8") as f:
        text = f.read()

    target_ver = tag.lstrip("v").strip()
    pattern = r"^##\s+\[(.*?)\](?:\s*-\s*([^\n\r]+))?"
    matches = list(re.finditer(pattern, text, re.MULTILINE))

    for i, m in enumerate(matches):
        version = m.group(1).strip().lstrip("v")
        if version == target_ver:
            date = m.group(2).strip() if m.group(2) else ""
            start = m.end()
            end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
            body = text[start:end].strip()
            return date, body

    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract release notes for a given tag from CHANGELOG.md.")
    parser.add_argument("--tag", required=True, help="Release tag or version (e.g. v1.1.6-exp.2 or 1.1.6-exp.2)")
    parser.add_argument("--changelog", type=Path, default=REPO_ROOT / "CHANGELOG.md", help="Path to CHANGELOG.md")
    parser.add_argument("--output", type=Path, default=None, help="Output file path (prints to stdout if omitted)")

    args = parser.parse_args()

    result = extract_notes(args.changelog, args.tag)
    if not result:
        print(f"Error: Version '{args.tag}' not found in '{args.changelog}'.", file=sys.stderr)
        return 1

    date, body = result

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(body + "\n")
        print(f"Wrote release notes for {args.tag} ({date}) to {args.output}")
    else:
        print(body)

    return 0


if __name__ == "__main__":
    sys.exit(main())
