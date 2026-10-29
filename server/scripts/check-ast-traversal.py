#!/usr/bin/env python3
"""
AST Traversal & Parent-Climb Quality Gate.
Enforces:
1. No unbounded root-climb loops (while/for (!ts_node_is_null(ts_node_parent(...))) / GetParent) across server/src/.
2. Zero raw ts_node_parent calls in hot-path collectors (LocalScopeCollector, SymbolCollector, SemanticTokensHandler).
3. Zero hardcoded engine-specific symbols ("BaseClass", "EHandle", etc.) outside EngineProfiles.cpp.
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

ROOT_CLIMB_PATTERNS = [
    re.compile(r"while\s*\(\s*!ts_node_is_null\s*\(\s*(ts_node_parent|parser::GetParent|GetParent)\s*\("),
    re.compile(r"for\s*\([^;]*;\s*!ts_node_is_null\s*\([^)]*\)\s*;\s*[^)]*(ts_node_parent|parser::GetParent|GetParent)\s*\("),
]

ENGINE_HARDCODED_SYMBOLS = [
    '"BaseClass"',
    '"EHandle"',
    '"HUDTextParams"',
    '"CBasePlayer"',
    '"CBaseEntity"',
    '"SetTimeout"',
    '"g_Scheduler"',
    '"CustomEntity"',
]

ENGINE_ALLOWED_FILES = {
    SERVER_SRC / "config" / "EngineProfiles.cpp",
    SERVER_SRC / "config" / "EngineRuleConfig.h",
}

def check_no_root_climb_loops():
    violations = []
    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            if any(p.search(line) for p in ROOT_CLIMB_PATTERNS):
                # Allow if depth-bounded
                if "depth" not in line and "kMax" not in line and "k_max" not in line:
                    violations.append((file_path, line_num, stripped))
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
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            if raw_parent_pattern.search(line):
                violations.append((file_path, line_num, stripped))
    return violations

def check_no_engine_hardcoding():
    violations = []
    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file() or file_path in ENGINE_ALLOWED_FILES:
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            for sym in ENGINE_HARDCODED_SYMBOLS:
                if sym in line:
                    violations.append((file_path, line_num, stripped))
    return violations

TYPE_CHECK_TARGET_FILES = [
    SERVER_SRC / "analysis" / "TypeConversionChecker.cpp",
    SERVER_SRC / "analysis" / "overload" / "OverloadTypeConversions.cpp",
    SERVER_SRC / "analysis" / "overload" / "ConversionRankingEngine.cpp",
    SERVER_SRC / "analysis" / "CallChecker.cpp",
]

FORBIDDEN_TYPE_PATTERNS = [
    (re.compile(r'"weakref"|"const_weakref"'), 'hardcoded "weakref" / "const_weakref" (use SymbolTable + generic template resolution)'),
    (re.compile(r'param\.baseTypeName\s*==\s*"string"'), 'hardcoded "string" check on param (use param.isOutRef / isInOutRef / isStandaloneRef)'),
    (re.compile(r'IsWeakrefToConstWeakref'), 'IsWeakrefToConstWeakref is banned (use generic template constructors)'),
]

def check_no_type_hardcoding():
    violations = []
    for file_path in TYPE_CHECK_TARGET_FILES:
        if not file_path.is_file():
            continue
        try:
            file_content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(file_content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            for pattern, reason in FORBIDDEN_TYPE_PATTERNS:
                if pattern.search(line):
                    violations.append((file_path, line_num, stripped, reason))
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

    engine_violations = check_no_engine_hardcoding()
    if engine_violations:
        has_errors = True
        print("[ERROR] Engine-specific symbols hardcoded outside EngineProfiles.cpp:")
        for f, line_num, line in engine_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    type_hardcoding_violations = check_no_type_hardcoding()
    if type_hardcoding_violations:
        has_errors = True
        print("[ERROR] Forbidden type hardcoding detected in Layer 2 (PROHIBITION 13):")
        for f, line_num, line, reason in type_hardcoding_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line} -> {reason}")

    if has_errors:
        sys.exit(1)

    print("[PASSED] AST Traversal, Parent-Climb, Engine Agnosticism, and Type Invariants verified (0 root-climb loops, 0 hot-path ts_node_parent calls, 0 engine hardcodings, 0 type hardcodings).")

if __name__ == "__main__":
    main()
