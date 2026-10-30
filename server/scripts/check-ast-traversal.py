#!/usr/bin/env python3
"""
AST Traversal, Parent-Climb & Layer Antipattern Quality Gate.
Enforces:
1. No unbounded root-climb loops (while/for (!ts_node_is_null(ts_node_parent(...))) / GetParent) across server/src/.
2. Zero raw ts_node_parent calls across server/src/ outside parser/ASTUtils.h.
3. Zero hardcoded engine-specific symbols ("BaseClass", "EHandle", etc.) outside EngineProfiles.cpp.
4. Zero hardcoded type names or parameter defaults ("weakref", "const_weakref", ="string", ="array") in Layer 2 & 3.
5. Zero heap string concatenations (+ "::" +) passed to SymbolTable lookups.
"""

import sys
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SERVER_SRC = REPO_ROOT / "server" / "src"

RAW_PARENT_ALLOWED_FILES = {
    SERVER_SRC / "parser" / "ASTUtils.h",
}

ROOT_CLIMB_PATTERNS = [
    re.compile(r"\b(?:for|while)\s*\([^)]*(?:ts_node_parent|parser::GetParent|GetParent)"),
]

PARENT_LOOP_ALLOWED_FILES = {
    SERVER_SRC / "parser" / "ASTUtils.h",
}

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

FORBIDDEN_TYPE_PATTERNS = [
    (re.compile(r'"weakref"|"const_weakref"'), 'hardcoded "weakref" / "const_weakref" (use SymbolTable + generic template resolution)'),
    (re.compile(r'param\.baseTypeName\s*==\s*"string"'), 'hardcoded "string" check on param (use param.isOutRef / isInOutRef / isStandaloneRef)'),
    (re.compile(r'IsWeakrefToConstWeakref'), 'IsWeakrefToConstWeakref is banned (use generic template constructors)'),
    (re.compile(r'(?:,|\()\s*(?:const\s+)?std::string_view\s+\w+\s*=\s*"(?:string|array)"'), 'forbidden default parameter ="string" or ="array" in function signature'),
]

SYMBOL_LOOKUP_CONCAT_PATTERNS = [
    re.compile(r'\.(?:HasSymbol|FindSymbols|FindSymbolsPtr)\s*\([^)]*\+\s*"::"'),
    re.compile(r'\.(?:HasSymbol|FindSymbols|FindSymbolsPtr)\s*\([^)]*"::"\s*\+'),
]

def check_no_root_climb_loops():
    """
    Enforces that NO loop (while/for) invokes GetParent or ts_node_parent outside ASTUtils.h.
    Any upward AST climbing must use parser::FindAncestorWithSymbol, parser::ForEachAncestor,
    or NodeIndex interval lookups in O(log K).
    """
    violations = []
    parent_climb_re = re.compile(r"\b(\w+)\s*=\s*(?:parser::)?GetParent\s*\(\1\)")
    loop_head_re = re.compile(r"\b(for|while)\s*\(")

    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file() or file_path in PARENT_LOOP_ALLOWED_FILES:
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue

        lines = content.splitlines()
        for idx, line in enumerate(lines):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if loop_head_re.search(line):
                if any(p.search(line) for p in ROOT_CLIMB_PATTERNS):
                    violations.append((file_path, idx + 1, stripped))
                    continue
                brace_count = 0
                seen_open = False
                for j in range(idx, min(idx + 35, len(lines))):
                    l = lines[j]
                    if parent_climb_re.search(l):
                        violations.append((file_path, idx + 1, stripped))
                        break
                    brace_count += l.count("{") - l.count("}")
                    if "{" in l:
                        seen_open = True
                    if seen_open and brace_count <= 0:
                        break
    return violations

def check_no_magic_depth_comparisons():
    """
    Enforces that NO magic depth numbers (depth < <number>) exist for upward AST traversal.
    """
    violations = []
    magic_depth_re = re.compile(r"\bdepth\s*<\s*\d+")
    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if magic_depth_re.search(line):
                violations.append((file_path, line_num, stripped))
    return violations

def check_no_raw_ts_node_parent():
    violations = []
    raw_parent_pattern = re.compile(r"\bts_node_parent\s*\(")
    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file() or file_path in RAW_PARENT_ALLOWED_FILES:
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
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

def check_no_type_hardcoding():
    violations = []
    scan_roots = [SERVER_SRC / "analysis", SERVER_SRC / "features"]
    for root in scan_roots:
        for file_path in root.rglob("*.[ch]*"):
            if not file_path.is_file() or file_path in ENGINE_ALLOWED_FILES:
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

def check_no_symbol_lookup_concat():
    violations = []
    scan_roots = [SERVER_SRC / "analysis", SERVER_SRC / "features"]
    for root in scan_roots:
        for file_path in root.rglob("*.[ch]*"):
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
                for pattern in SYMBOL_LOOKUP_CONCAT_PATTERNS:
                    if pattern.search(line):
                        violations.append((file_path, line_num, stripped))
    return violations

def check_no_ts_node_type():
    violations = []
    ts_node_type_pattern = re.compile(r"\bts_node_type\s*\(")
    scan_roots = [SERVER_SRC / "analysis", SERVER_SRC / "features"]
    for root in scan_roots:
        for file_path in root.rglob("*.[ch]*"):
            if not file_path.is_file() or file_path.name == "DoxygenMarkdown.cpp":
                continue
            try:
                content = file_path.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            for line_num, line in enumerate(content.splitlines(), start=1):
                stripped = line.strip()
                if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                    continue
                if ts_node_type_pattern.search(line):
                    violations.append((file_path, line_num, stripped))
    return violations

def check_no_parser_fields():
    violations = []
    fields_pattern = re.compile(r"\bfields::\w+")
    scan_roots = [SERVER_SRC / "analysis", SERVER_SRC / "features"]
    for root in scan_roots:
        for file_path in root.rglob("*.[ch]*"):
            if not file_path.is_file():
                continue
            try:
                content = file_path.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            for line_num, line in enumerate(content.splitlines(), start=1):
                stripped = line.strip()
                if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                    continue
                if fields_pattern.search(line):
                    violations.append((file_path, line_num, stripped))
    return violations

def check_no_scope_string_concat_in_rules():
    violations = []
    scope_concat_patterns = [
        re.compile(r'\+\s*"::"'),
        re.compile(r'"::"\s*\+'),
    ]
    rules_root = SERVER_SRC / "analysis" / "rules"
    for file_path in rules_root.rglob("*.[ch]*"):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if any(p.search(line) for p in scope_concat_patterns):
                violations.append((file_path, line_num, stripped))
    return violations

def check_no_manual_scope_find():
    violations = []
    scope_find_pattern = re.compile(r'\.(?:find|rfind)\s*\(\s*"::"')
    allowed_files = {
        SERVER_SRC / "utils" / "ScopedNameUtils.h",
        SERVER_SRC / "utils" / "ScopedNameUtils.cpp",
    }
    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file() or file_path in allowed_files:
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if scope_find_pattern.search(line):
                violations.append((file_path, line_num, stripped))
    return violations

def check_no_manual_scope_string_ops():
    violations = []
    scope_ops_pattern = re.compile(r'\.(?:starts_with|ends_with)\s*\(\s*"::"\s*\)|\+\s*"::"|"::"\s*\+')
    allowed_files = {
        SERVER_SRC / "utils" / "ScopedNameUtils.h",
        SERVER_SRC / "utils" / "ScopedNameUtils.cpp",
        SERVER_SRC / "lsp" / "SignatureFormatter.cpp",
    }
    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file() or file_path in allowed_files:
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if scope_ops_pattern.search(line):
                violations.append((file_path, line_num, stripped))
    return violations

def check_no_operator_node_text():
    violations = []
    op_text_pattern = re.compile(r'(?:NodeText|GetNodeText)\s*\(\s*(?:opNode|operatorNode)\b')
    scan_roots = [SERVER_SRC / "analysis", SERVER_SRC / "features"]
    for root in scan_roots:
        for file_path in root.rglob("*.[ch]*"):
            if not file_path.is_file():
                continue
            try:
                content = file_path.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            for line_num, line in enumerate(content.splitlines(), start=1):
                stripped = line.strip()
                if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                    continue
                if op_text_pattern.search(line):
                    violations.append((file_path, line_num, stripped))
    return violations

def check_no_keyword_fallbacks():
    violations = []
    kw_fallback_pattern = re.compile(r'(?:NodeText|GetNodeText)\s*\([^)]*\)\s*==\s*"const"')
    scan_roots = [SERVER_SRC / "analysis", SERVER_SRC / "features"]
    for root in scan_roots:
        for file_path in root.rglob("*.[ch]*"):
            if not file_path.is_file():
                continue
            try:
                content = file_path.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            for line_num, line in enumerate(content.splitlines(), start=1):
                stripped = line.strip()
                if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                    continue
                if kw_fallback_pattern.search(line):
                    violations.append((file_path, line_num, stripped))
    return violations

def check_no_node_type_wrapper():
    violations = []
    pattern = re.compile(r'(?<!\w)NodeType\s*\(')
    scan_roots = [SERVER_SRC / "analysis", SERVER_SRC / "features", SERVER_SRC / "lsp"]
    for root in scan_roots:
        for file_path in root.rglob("*.[ch]*"):
            if not file_path.is_file():
                continue
            try:
                content = file_path.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            for line_num, line in enumerate(content.splitlines(), start=1):
                stripped = line.strip()
                if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                    continue
                if pattern.search(line):
                    violations.append((file_path, line_num, stripped))
    return violations

def check_no_modifier_string_literals():
    violations = []
    pattern = re.compile(r'"(?:class|interface|enum|funcdef|shared)[\s\t]"')
    allowed_files = {
        SERVER_SRC / "analysis" / "SignatureFormatter.cpp",
        SERVER_SRC / "analysis" / "EngineConfigConverter.cpp",
    }
    for file_path in (SERVER_SRC / "analysis").rglob("*.[ch]*"):
        if not file_path.is_file() or file_path in allowed_files:
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_num, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if pattern.search(line):
                violations.append((file_path, line_num, stripped))
    return violations

def main():
    has_errors = False
    
    root_climb_violations = check_no_root_climb_loops()
    if root_climb_violations:
        has_errors = True
        print("[ERROR] AST upward climb loops detected outside parser/ASTUtils.h (use parser::FindAncestorWithSymbol, parser::ForEachAncestor, or NodeIndex):")
        for f, line_num, line in root_climb_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    magic_depth_violations = check_no_magic_depth_comparisons()
    if magic_depth_violations:
        has_errors = True
        print("[ERROR] Magic depth comparisons (depth < <number>) detected in server/src/ (use parser::FindAncestorWithSymbol or parser::ForEachAncestor in ASTUtils.h):")
        for f, line_num, line in magic_depth_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    raw_parent_violations = check_no_raw_ts_node_parent()
    if raw_parent_violations:
        has_errors = True
        print("[ERROR] Raw ts_node_parent calls detected outside parser/ASTUtils.h (use parser::GetParent):")
        for f, line_num, line in raw_parent_violations:
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
        print("[ERROR] Forbidden type hardcoding detected in Layer 2 or Layer 3 (PROHIBITION 13):")
        for f, line_num, line, reason in type_hardcoding_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line} -> {reason}")

    concat_violations = check_no_symbol_lookup_concat()
    if concat_violations:
        has_errors = True
        print("[ERROR] Heap string concatenation (+ '::' +) detected in SymbolTable lookups (use FindMemberSymbolPtr / FindMemberSymbols / HasScopedSymbol):")
        for f, line_num, line in concat_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    scope_concat_violations = check_no_scope_string_concat_in_rules()
    if scope_concat_violations:
        has_errors = True
        print("[ERROR] Scoped string concatenation (+ '::' or '::' +) detected in analysis rules (use utils::ScopedNameUtils or SymbolTable):")
        for f, line_num, line in scope_concat_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    ts_node_type_violations = check_no_ts_node_type()
    if ts_node_type_violations:
        has_errors = True
        print("[ERROR] Raw ts_node_type calls detected in Layer 2/3 (use ts_node_symbol and parser::GetGrammarSymbols()):")
        for f, line_num, line in ts_node_type_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    parser_fields_violations = check_no_parser_fields()
    if parser_fields_violations:
        has_errors = True
        print("[ERROR] parser::fields:: detected in Layer 2/3 (use gs.fieldX from parser::GetGrammarSymbols()):")
        for f, line_num, line in parser_fields_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    manual_scope_violations = check_no_manual_scope_find()
    if manual_scope_violations:
        has_errors = True
        print("[ERROR] Manual .find('::') or .rfind('::') detected outside ScopedNameUtils (use utils::ScopedNameUtils):")
        for f, line_num, line in manual_scope_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    manual_scope_ops_violations = check_no_manual_scope_string_ops()
    if manual_scope_ops_violations:
        has_errors = True
        print("[ERROR] Manual scope operations (+ '::', '::' +, .starts_with('::'), .ends_with('::')) detected outside ScopedNameUtils:")
        for f, line_num, line in manual_scope_ops_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    op_text_violations = check_no_operator_node_text()
    if op_text_violations:
        has_errors = True
        print("[ERROR] NodeText/GetNodeText called on operator node (use ts_node_symbol with gs.tok*):")
        for f, line_num, line in op_text_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    kw_fallback_violations = check_no_keyword_fallbacks()
    if kw_fallback_violations:
        has_errors = True
        print("[ERROR] NodeText == 'const' fallback detected (use ts_node_symbol == gs.tokConst):")
        for f, line_num, line in kw_fallback_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    node_type_violations = check_no_node_type_wrapper()
    if node_type_violations:
        has_errors = True
        print("[ERROR] Deprecated NodeType(node) wrapper calls detected in Layer 2/3/4 (use ts_node_symbol with parser::GetGrammarSymbols()):")
        for f, line_num, line in node_type_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    modifier_violations = check_no_modifier_string_literals()
    if modifier_violations:
        has_errors = True
        print("[ERROR] Modifier string literals ('class ', 'interface ', etc.) detected in server/src/analysis/ (use parser::keywords or AST grammar tokens):")
        for f, line_num, line in modifier_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    if has_errors:
        sys.exit(1)

    print("[PASSED] AST Traversal, Parent Isolation, Agnosticism, Type & Scope Invariants verified (0 root-climb loops, 0 raw ts_node_parent calls, 0 ts_node_type in layers 2/3, 0 NodeType in layers 2/3/4, 0 modifier string literals in analysis, 0 parser::fields:: in layers 2/3, 0 manual scope finds, 0 op NodeText, 0 kw fallbacks, 0 scope concatenations in rules, 0 engine hardcodings, 0 type hardcodings).")

if __name__ == "__main__":
    main()
