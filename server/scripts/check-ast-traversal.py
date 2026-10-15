#!/usr/bin/env python3
"""
AST Traversal & Parent-Climb Quality Gate.
Enforces:
1. No unbounded root-climb loops (while (!ts_node_is_null(ts_node_parent(...))) / GetParent) across server/src/.
2. Zero raw ts_node_parent calls in hot-path collectors (LocalScopeCollector, SymbolCollector, SemanticTokensHandler).
"""

import sys
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SERVER_SRC = REPO_ROOT / "server" / "src"

HOT_PATH_FILES = [
    SERVER_SRC / "analysis" / "LocalScopeCollector.h",
    SERVER_SRC / "analysis" / "LocalScopeCollector.cpp",
    SERVER_SRC / "analysis" / "SymbolCollector.h",
    SERVER_SRC / "analysis" / "SymbolCollector.cpp",
    SERVER_SRC / "features" / "semantic_tokens" / "SemanticTokensHandler.cpp",
]

ROOT_CLIMB_PATTERN = re.compile(r"while\s*\(\s*!ts_node_is_null\s*\(\s*(ts_node_parent|parser::GetParent|GetParent)\s*\(")

def check_no_root_climb_loops():
    violations = []
    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception as e:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            if ROOT_CLIMB_PATTERN.search(line):
                violations.append((file_path, line_num, line.strip()))
    return violations

def check_hot_path_parent_calls():
    violations = []
    raw_parent_pattern = re.compile(r"\bts_node_parent\s*\(")
    for file_path in HOT_PATH_FILES:
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            # Exclude comments
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            if raw_parent_pattern.search(line):
                violations.append((file_path, line_num, stripped))
    return violations

def main():
    has_errors = False
    
    root_climb_violations = check_no_root_climb_loops()
    if root_climb_violations:
        has_errors = True
        print("[ERROR] Unbounded root-climb loops detected in server/src/ (use parser::GetRootNode(node) in O(1)):")
        for f, line_num, line in root_climb_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    hot_path_violations = check_hot_path_parent_calls()
    if hot_path_violations:
        has_errors = True
        print("[ERROR] Raw ts_node_parent calls detected in hot-path collectors (use parser::GetParent or O(1) symbols):")
        for f, line_num, line in hot_path_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    if has_errors:
        sys.exit(1)

    print("[PASSED] AST Traversal and Parent-Climb Invariants verified (0 root-climb loops, 0 hot-path ts_node_parent calls).")

if __name__ == "__main__":
    main()
