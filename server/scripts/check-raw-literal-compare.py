#!/usr/bin/env python3
"""Ratchet gate for AGENTS.md PROHIBITION 16 (no raw literal comparisons).

Counts comparisons of type names, keywords and literals against raw string literals, and
`find('<')` / `starts_with("const")` template and qualifier probes, per source file. A file's count
may only go down. A file with no baseline entry must have none. `--update` rewrites the baseline
after a migration lowered counts; it refuses to raise any count.
"""
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SRC = REPO_ROOT / "server" / "src"
BASELINE = REPO_ROOT / "server" / "rules" / "raw-literal-baseline.json"
SCAN_DIRS = ("analysis", "features", "lsp")
EXEMPT = ("analysis/formatting/ClangFormat",)

LITERALS = (
    "auto|void|null|true|false|this|super|const|bool|int|uint|float|double|string|any|\\?"
)
PATTERN = re.compile(
    r'[!=]=\s*"(?:' + LITERALS + r')"'
    r"|find\('<'\)"
    r'|starts_with\("const'
)


def count_file(path: Path) -> int:
    total = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        total += len(PATTERN.findall(line))
    return total


def scan() -> dict:
    counts = {}
    for scan_dir in SCAN_DIRS:
        for path in sorted((SRC / scan_dir).rglob("*")):
            if path.suffix not in (".cpp", ".h"):
                continue
            rel = path.relative_to(SRC).as_posix()
            if rel.startswith(EXEMPT):
                continue
            n = count_file(path)
            if n:
                counts[rel] = n
    return counts


def main() -> int:
    update = "--update" in sys.argv
    current = scan()
    baseline = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    if "--init" in sys.argv:
        BASELINE.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        print(f"baseline written: {sum(current.values())} sites in {len(current)} files")
        return 0

    raised = {f: (baseline.get(f, 0), n) for f, n in current.items() if n > baseline.get(f, 0)}
    if raised:
        print("[FAILED] raw literal comparisons increased (AGENTS.md PROHIBITION 16):")
        for f, (old, new) in sorted(raised.items()):
            print(f"  {f}: {old} -> {new}")
        print("Use TypeKind, parser::keywords, GrammarSymbols, TypeSanitization or ParseTemplateType.")
        return 1

    lowered = {f for f in baseline if current.get(f, 0) < baseline[f]}
    if lowered and update:
        BASELINE.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        print(f"baseline lowered for {len(lowered)} files")
    elif lowered:
        print(f"[NOTE] {len(lowered)} files improved; run with --update to lock it in")
    print(f"raw literal comparisons: {sum(current.values())} sites in {len(current)} files (baseline {sum(baseline.values())})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
