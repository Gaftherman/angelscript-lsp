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
SERVER_TESTS = REPO_ROOT / "server" / "tests"

RAW_PARENT_ALLOWED_FILES = {
    SERVER_SRC / "parser" / "ASTUtils.h",
}

ROOT_CLIMB_PATTERNS = [
    re.compile(r"\b(?:for|while)\s*\([^)]*(?:ts_node_parent|parser::GetParent|GetParent)"),
]

PARENT_LOOP_ALLOWED_FILES = {
    SERVER_SRC / "parser" / "ASTUtils.h",
}

INDEXED_CHECKERS = (
    SERVER_SRC / "analysis" / "checkers" / "BinaryOperatorChecker.cpp",
    SERVER_SRC / "analysis" / "checkers" / "BinaryOperatorHelpers.cpp",
    SERVER_SRC / "analysis" / "checkers" / "ComparisonOperatorChecker.cpp",
    SERVER_SRC / "analysis" / "checkers" / "HandleComparisonChecker.cpp",
    SERVER_SRC / "analysis" / "checkers" / "RepeatedConversionChecker.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionAssignments.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionAuto.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionChecker.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionConditions.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionConstruction.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionCore.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionDeclarations.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionExpressions.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionFuncdefs.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionHierarchy.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionNumericWarnings.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionProperties.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionReturns.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionSymbols.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionTernaryForeach.cpp",
    SERVER_SRC / "analysis" / "checkers" / "TypeConversionValueType.cpp",
    SERVER_SRC / "analysis" / "checkers" / "null_safety" / "NullSafetyChecker.cpp",
    SERVER_SRC / "analysis" / "checkers" / "null_safety" / "NullSafetyCondition.cpp",
    SERVER_SRC / "analysis" / "checkers" / "null_safety" / "NullSafetyDecl.cpp",
    SERVER_SRC / "analysis" / "checkers" / "null_safety" / "NullSafetyExpr.cpp",
    SERVER_SRC / "analysis" / "checkers" / "null_safety" / "NullSafetyParam.cpp",
    SERVER_SRC / "analysis" / "types" / "TypeExtraction.cpp",
)

DIRECT_AST_NAVIGATION = re.compile(
    r"\b(?:ts_(?:tree|query)_cursor_\w+|ts_node_(?:parent|(?:named_)?child\w*|descendant\w*|"
    r"(?:next|prev)_(?:named_)?sibling)|parser::(?:GetParent|GetChildByField|GetCallCallee|"
    r"FindAncestor\w*|FindChildWithSymbol|ForEach(?:Ancestor|(?:Named)?ChildNode\w*)|UnwrapParentheses))\s*\("
)

ENGINE_HARDCODED_SYMBOLS = [
    '"BaseClass"',
    '"self"',
    '"EHandle"',
    '"HUDTextParams"',
    '"CBasePlayer"',
    '"CBaseEntity"',
    '"SetTimeout"',
    '"g_Scheduler"',
    '"CustomEntity"',
]

ENGINE_ALLOWED_FILES = {
    SERVER_SRC / "analysis" / "types" / "EngineProfiles.cpp",
}

BANNED_IDENTIFIER_PATTERNS = [
    (re.compile(r"\bk_baseClassIdentifier\b"), "banned identifier k_baseClassIdentifier (eradicated in favor of dynamic baseClassDelegateProperties)"),
    (re.compile(r"\bk_selfIdentifier\b"), "banned identifier k_selfIdentifier (eradicated in favor of generic SymbolTable / property accessors)"),
    (re.compile(r"\bResolveSelfOrBaseClass\b"), "banned function ResolveSelfOrBaseClass (eradicated in favor of generic ResolveBaseClass / delegate properties)"),
    (re.compile(r"\bResolveSelfOrBaseReceiver\b"), "banned function ResolveSelfOrBaseReceiver (eradicated in favor of generic ResolveDelegatePropertyReceiver)"),
]

FORBIDDEN_TYPE_PATTERNS = [
    (re.compile(r'"weakref"|"const_weakref"'), 'hardcoded "weakref" / "const_weakref" (use SymbolTable + generic template resolution)'),
    (re.compile(r'param\.baseTypeName\s*==\s*"string"'), 'hardcoded "string" check on param (use param.isOutRef / isInOutRef / isStandaloneRef)'),
    (re.compile(r'IsWeakrefToConstWeakref'), 'IsWeakrefToConstWeakref is banned (use generic template constructors)'),
    (re.compile(r'(?:,|\()\s*(?:const\s+)?std::string_view\s+\w+\s*=\s*"(?:string|array)"'), 'forbidden default parameter ="string" or ="array" in function signature'),
    (re.compile(r'"init_list"'), 'hardcoded "init_list" literal (use constants::k_initializerListTypeSentinel)'),
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

def check_indexed_checkers_use_facts():
    violations = []
    for path in INDEXED_CHECKERS:
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            code = line.strip()
            if code.startswith(("//", "*", "/*")):
                continue
            if DIRECT_AST_NAVIGATION.search(code):
                violations.append((path, line_number, code))
    return violations


def check_single_syntax_producer():
    """Detects direct construction of a document syntax revision in LSP code."""
    state = SERVER_SRC / "lsp" / "state"
    producer_files = {state / "SyntaxRevisionCache.cpp", state / "SyntaxRevisionCache.h"}
    construction = re.compile(
        r"\b(?:make_shared|allocate_shared|make_unique)\s*<\s*(?:const\s+)?ParsedDocumentSyntax\b|"
        r"\bnew\s+ParsedDocumentSyntax\b|\bParsedDocumentSyntax\s*(?:\{|\()|"
        r"\bParsedDocumentSyntax\s+\w+\s*\{"
    )
    for path in (SERVER_SRC / "lsp").rglob("*.[ch]*"):
        if not path.is_file() or path in producer_files:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            code = line.strip()
            if not code.startswith(("//", "/*", "*")) and construction.search(code):
                yield path, number, code


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
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            code_part = line.split("//")[0].strip()
            for sym in ENGINE_HARDCODED_SYMBOLS:
                if sym in code_part:
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
        SERVER_SRC / "analysis" / "documentation" / "SignatureFormatter.cpp",
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
        SERVER_SRC / "analysis" / "documentation" / "SignatureFormatter.cpp",
        SERVER_SRC / "analysis" / "types" / "EngineConfigConverter.cpp",
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

def check_no_parser_nodes_in_layers_2_3_4():
    """
    Enforces that NO parser::nodes:: is referenced in Layer 2 (analysis), Layer 3 (features), or Layer 4 (lsp).
    All AST node type checks must use precompiled TSSymbols via parser::GetGrammarSymbols().
    """
    violations = []
    pattern = re.compile(r"\bparser::nodes::")
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

def check_no_cursor_field_name():
    """
    Enforces that NO ts_tree_cursor_current_field_name is called in Layer 2, 3, or 4.
    Must use ts_tree_cursor_current_field_id with cached TSFieldId symbols.
    """
    violations = []
    pattern = re.compile(r"\bts_tree_cursor_current_field_name\b")
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

def check_no_depth_number_comparisons():
    """
    Enforces that NO magic depth numbers (depth > N, depth >= N) or k_maxAstDepth exist in Layers 2, 3, or 4.
    Sub-tree traversals must be flat with iterative worklists rather than recursive depth caps.
    """
    violations = []
    pattern = re.compile(r"\bk_maxAstDepth\b|\bdepth\s*(?:>=?|>|<=)\s*(?:[1-9]\d*|k_maxAstDepth)")
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

def check_no_duplicate_to_source_range():
    """
    Enforces that NO duplicate ToSourceRange(TSNode) definitions exist in .cpp files.
    The centralized canonical implementation lives in SymbolTable.h.
    """
    violations = []
    pattern = re.compile(r"\bSourceRange\s+ToSourceRange\s*\(")
    scan_roots = [SERVER_SRC / "analysis", SERVER_SRC / "features", SERVER_SRC / "lsp"]
    for root in scan_roots:
        for file_path in root.rglob("*.cpp"):
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

def check_no_for_each_descendant_node():
    """
    Enforces that ForEachDescendantNode is eradicated across the entire server/src/ codebase.
    """
    violations = []
    pattern = re.compile(r"\bForEachDescendantNode\b")
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
            if pattern.search(line):
                violations.append((file_path, line_num, stripped))
    return violations

def check_no_banned_identifiers():
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
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            for pat, reason in BANNED_IDENTIFIER_PATTERNS:
                if pat.search(line):
                    violations.append((file_path, line_num, stripped, reason))
    return violations

def check_no_baseclass_in_tests():
    violations = []
    allowed_pattern = re.compile(r'GenerateRandomSymbolName\s*\(\s*"BaseClass"\s*\)')
    for file_path in SERVER_TESTS.rglob("*.cpp"):
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
            if '"BaseClass"' in line:
                if not allowed_pattern.search(line):
                    violations.append((file_path, line_num, stripped))
    return violations

def check_no_analysis_ast_utils():
    """Enforces that analysis/ASTUtils.h does not exist (unified into parser/ASTUtils.h)."""
    target = SERVER_SRC / "analysis" / "ASTUtils.h"
    if target.exists():
        return [target]
    return []

MANUAL_QUALIFIER_ALLOWED_FILES = {
    SERVER_SRC / "analysis" / "types" / "TypeExtraction.cpp",
    SERVER_SRC / "analysis" / "types" / "TypeExtraction.h",
    SERVER_SRC / "analysis" / "types" / "TypeSanitization.cpp",
    SERVER_SRC / "analysis" / "types" / "TypeSanitization.h",
}

MANUAL_QUALIFIER_PATTERNS = [
    (re.compile(r'\.(?:find|ends_with)\s*\(\s*[\'"]@[\'"]'), "manual '@' search (use analysis::IsHandleTypeView)"),
    (re.compile(r'\.back\(\)\s*==\s*[\'"]@[\'"]'), "manual back() == '@' (use analysis::IsHandleTypeView)"),
    (re.compile(r'\.(?:starts_with|find)\s*\(\s*"const "'), 'manual "const " search (use analysis::IsConstTypeView or StripLeadingConst)'),
    (re.compile(r'\.find\s*\(\s*"&out"'), 'manual "&out" search (use param.isOutRef / IsReferenceTypeView)'),
    (re.compile(r'\.find\s*\(\s*"&inout"'), 'manual "&inout" search (use param.isInOutRef / IsReferenceTypeView)'),
    (re.compile(r'\.find\s*\(\s*"&in"'), 'manual "&in" search (use param.modifier / IsReferenceTypeView)'),
]

def check_no_manual_type_qualifier_parsing():
    """
    Enforces that NO manual type qualifier reparsing is performed in Layer 2 (analysis)
    or Layer 3 (features). All qualifier checks must read precalculated Pass-1 flags
    or use canonical zero-allocation helpers in analysis/TypeSanitization.h.
    """
    violations = []
    target_dirs = [SERVER_SRC / "analysis", SERVER_SRC / "features"]
    for d in target_dirs:
        for file_path in d.rglob("*.[ch]*"):
            if not file_path.is_file() or file_path in MANUAL_QUALIFIER_ALLOWED_FILES:
                continue
            try:
                content = file_path.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue

            for idx, line in enumerate(content.splitlines(), 1):
                stripped = line.strip()
                if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                    continue
                for pat, desc in MANUAL_QUALIFIER_PATTERNS:
                    if pat.search(line):
                        violations.append((file_path, idx, stripped, desc))
                        break
    return violations

def check_no_anonymous_namespaces():
    """
    Enforces that zero anonymous namespaces (namespace {) exist across server/src/.
    All internal helpers must be file-scope static or placed in <module>_detail sub-namespaces.
    """
    violations = []
    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            if re.search(r"\bnamespace\s*\{", line):
                violations.append((file_path, idx, line.strip()))
    return violations


def check_no_using_namespace_or_declarations():
    """
    Enforces that zero 'using namespace' directives and zero 'using <ns>::<symbol>;'
    declarations exist across server/src/. All namespace access must be fully qualified.
    Only type aliases ('using Alias = Type;') are permitted.
    Also enforces zero namespace aliases (namespace X = Y;) across server/src/, server/tests/, and server/tools/,
    and zero 'using namespace' in test headers (server/tests/**/*.h) or tools (server/tools/**).
    """
    violations = []
    using_ns_re = re.compile(r"\busing\s+namespace\b")
    using_sym_re = re.compile(r"\busing\s+(?![^;=]*=)[a-zA-Z0-9_:,\s]+;")
    ns_alias_re = re.compile(r"\bnamespace\s+[A-Za-z0-9_]+\s*=")

    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*") or stripped.startswith(";"):
                continue
            if using_ns_re.search(stripped) or using_sym_re.search(stripped) or ns_alias_re.search(stripped):
                violations.append((file_path, idx, stripped))

    # Test headers and tools must have zero using namespace or namespace aliases
    server_tools = REPO_ROOT / "server" / "tools"
    for file_path in list(SERVER_TESTS.rglob("*.h")) + list(server_tools.rglob("*.[ch]*")):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*") or stripped.startswith(";"):
                continue
            if using_ns_re.search(stripped) or ns_alias_re.search(stripped):
                violations.append((file_path, idx, stripped))

    # All test cpp files must have zero namespace aliases (namespace X = Y;)
    for file_path in SERVER_TESTS.rglob("*.cpp"):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*") or stripped.startswith(";"):
                continue
            if ns_alias_re.search(stripped):
                violations.append((file_path, idx, stripped))

    return violations


check_no_using_directives_or_declarations = check_no_using_namespace_or_declarations


def check_no_doxygen_slop():
    """
    Enforces that zero mechanical or robotic AI slop filler templates exist in Doxygen comments across server/src/.
    Specifically checks for:
    1. '[REFACTOR-CANDIDATE]'
    2. '@brief Represents <symbol>'
    3. '@brief Helper structure' / '@brief Local helper'
    4. 'True if condition met; otherwise false.'
    5. 'Result of the operation.' / 'Calculated or extracted result of the operation.'
    6. '@param... Parameter/Input <name>.'
    7. Orphaned analysis note fragments outside of @note.
    """
    violations = []
    refactor_cand_re = re.compile(r"\[REFACTOR-CANDIDATE\]", re.I)
    orphan_reeval_re = re.compile(r"Manually re-evaluates", re.I)
    orphan_pass1_re = re.compile(r"candidate for direct Pass 1", re.I)
    orphan_unif_re = re.compile(r"Candidate for unification", re.I)
    orphan_type_extract_re = re.compile(r"TypeExtraction::ExtractTypeInfo", re.I)
    orphan_handle_type_re = re.compile(r"LocalDefinition::isHandleType", re.I)
    orphan_symbol_mod_re = re.compile(r"Symbol::modifiers\.", re.I)
    slop_brief_re = re.compile(r"@brief\s+(?:Represents\b|Helper\s+structure\b|Local\s+helper\b)", re.I)
    slop_param_re = re.compile(r"@param(\[[^\]]*\])?\s+(\w+)\s+(Parameter|Input)\s+\2\b", re.I)
    slop_cond_re = re.compile(r"true if condition met;\s*otherwise false", re.I)
    slop_res_re = re.compile(r"result of the operation", re.I)
    slop_unify_re = re.compile(r"(?:candidate\s+)?for\s+unification\s+in", re.I)
    slop_overlap_re = re.compile(r"structural overlap\s*\(\d+%\)", re.I)

    for file_path in SERVER_SRC.rglob("*.[ch]*"):
        if not file_path.is_file():
            continue
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            cleaned_tag = stripped.lstrip("* ").strip()
            is_comment = stripped.startswith("*") or stripped.startswith("//") or stripped.startswith("/*")
            if refactor_cand_re.search(stripped):
                violations.append((file_path, idx, stripped, "Forbidden [REFACTOR-CANDIDATE] tag"))
            elif orphan_reeval_re.search(stripped):
                violations.append((file_path, idx, stripped, "Orphaned manual re-evaluation note fragment"))
            elif orphan_pass1_re.search(stripped):
                violations.append((file_path, idx, stripped, "Orphaned pass 1 candidate note fragment"))
            elif is_comment and (orphan_type_extract_re.search(stripped) or orphan_handle_type_re.search(stripped) or orphan_symbol_mod_re.search(stripped)):
                violations.append((file_path, idx, stripped, "Orphaned type/symbol analysis note fragment"))
            elif slop_brief_re.search(stripped):
                violations.append((file_path, idx, stripped, "Mechanical @brief slop template"))
            elif slop_cond_re.search(stripped):
                violations.append((file_path, idx, stripped, "Mechanical filler condition description"))
            elif slop_res_re.search(stripped):
                violations.append((file_path, idx, stripped, "Mechanical filler return description"))
            elif slop_param_re.search(stripped):
                violations.append((file_path, idx, stripped, "Mechanical filler parameter description"))
            elif (slop_unify_re.search(stripped) or orphan_unif_re.search(stripped)) and not cleaned_tag.startswith("@note"):
                violations.append((file_path, idx, stripped, "Orphaned refactoring note fragment"))
            elif slop_overlap_re.search(stripped) and not cleaned_tag.startswith("@note"):
                violations.append((file_path, idx, stripped, "Orphaned structural overlap note"))
    return violations


def check_no_detail_namespaces():
    """
    Enforces that NO namespace *_detail exists across server/src/.
    All helper structures must reside in canonical headers or private class scopes.
    """
    violations = []
    detail_ns_re = re.compile(r"\bnamespace\s+([A-Za-z0-9_]*_detail)\b")
    for fpath in SERVER_SRC.rglob("*.[ch]*"):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if detail_ns_re.search(stripped):
                violations.append((fpath, idx, stripped))
    return violations


def check_no_detail_using_aliases():
    """
    Enforces that NO 'using ... = ..._detail::...' aliases exist across server/src/.
    """
    violations = []
    using_detail_re = re.compile(r"\busing\s+\w+\s*=\s*[A-Za-z0-9_]*_detail::")
    for fpath in SERVER_SRC.rglob("*.[ch]*"):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if using_detail_re.search(stripped):
                violations.append((fpath, idx, stripped))
    return violations


def check_no_cpp_struct_declarations():
    """
    Enforces that ZERO direct struct declarations exist inside .cpp files in Layer 2 (analysis/)
    and Layer 3 (features/). All structures must reside in canonical module headers.
    """
    violations = []
    struct_decl_re = re.compile(r"\bstruct\s+([A-Za-z0-9_]+)\s*(?:final\s*)?(?::[^{;]+)?\{")
    scan_dirs = [SERVER_SRC / "analysis", SERVER_SRC / "features"]
    for sdir in scan_dirs:
        for fpath in sdir.rglob("*.cpp"):
            if not fpath.is_file():
                continue
            try:
                content = fpath.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            cleaned = re.sub(r"/\*.*?\*/", "", content, flags=re.DOTALL)
            cleaned = re.sub(r"//.*$", "", cleaned, flags=re.MULTILINE)
            for m in struct_decl_re.finditer(cleaned):
                line_num = content[:m.start()].count("\n") + 1
                violations.append((fpath, line_num, f"struct {m.group(1)}", m.group(1)))
    return violations


def check_cpp_structs_odr_safety():
    """
    Enforces ODR safety for all struct and class definitions in .cpp files:
    1. Zero struct/class name collisions across different .cpp files.
    2. Zero struct/class name collisions with header (.h) definitions.
    """
    type_decl_re = re.compile(r"\b(?:struct|class)\s+([A-Za-z0-9_]+)\s*(?:final\s*)?(?::[^{]+)?\{")

    headers = list(SERVER_SRC.rglob("*.h"))
    h_struct_names = set()
    for h in headers:
        try:
            txt = h.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for m in type_decl_re.finditer(txt):
            h_struct_names.add(m.group(1))

    cpp_files = sorted(SERVER_SRC.rglob("*.cpp"))
    cpp_structs = {}
    for fpath in cpp_files:
        try:
            txt = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for m in type_decl_re.finditer(txt):
            s_name = m.group(1)
            line_num = txt[:m.start()].count("\n") + 1
            cpp_structs.setdefault(s_name, []).append((fpath, line_num))

    cross_collisions = [
        (name, locs) for name, locs in cpp_structs.items() if len(locs) > 1
    ]
    header_collisions = [
        (name, locs) for name, locs in cpp_structs.items() if name in h_struct_names
    ]

    return {
        "cross_collisions": cross_collisions,
        "header_collisions": header_collisions,
    }


def check_no_stacked_doxygen_blocks():
    r"""
    Check 24: Enforces Prohibition of stacked or consecutive Doxygen comment blocks (*/\s*/**).
    Each declaration must have at most one cohesive Doxygen block.
    """
    violations = []
    stacked_re = re.compile(r"\*/\s*/\*\*")
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file() or fpath.name == "DoxygenMarkdown.cpp":
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for m in stacked_re.finditer(content):
            line_num = content[:m.start()].count("\n") + 1
            violations.append((fpath, line_num, "Stacked Doxygen comment blocks (*/ ... /**)"))
    return violations


def check_no_decorative_comment_banners():
    """
    Check 25: Enforces Prohibition of decorative comment dividers/banners (// ===..., // ---..., // ***...).
    """
    violations = []
    banner_re = re.compile(r"^//\s*[=\-*]{4,}\s*$")
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            if banner_re.match(stripped):
                violations.append((fpath, idx, stripped))
    return violations


def check_no_workflow_narration_comments():
    """
    Check 26: Enforces Prohibition of step-by-step workflow narration comments (// Step 1, // First,, // Finally,).
    """
    violations = []
    narration_re = re.compile(r"^//\s*(?:Step\s+\d+|First,|Finally,)\b", re.IGNORECASE)
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            if narration_re.search(stripped):
                violations.append((fpath, idx, stripped))
    return violations


def check_no_over_explained_doxygen_prose():
    """
    Check 27: Enforces concise technical documentation in Doxygen blocks (preventing AI essays).
    No single uninterrupted free-prose paragraph may exceed 7 lines.
    """
    violations = []
    doxygen_re = re.compile(r"/\*\*(.*?)\*/", re.DOTALL)
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file() or fpath.name == "DoxygenMarkdown.cpp":
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for m in doxygen_re.finditer(content):
            block = m.group(1)
            lines = [l.strip().lstrip("*").strip() for l in block.splitlines()]
            cur_prose = 0
            max_in_block = 0
            for l in lines:
                if not l or l.startswith("@"):
                    cur_prose = 0
                    continue
                cur_prose += 1
                if cur_prose > max_in_block:
                    max_in_block = cur_prose
            if max_in_block >= 3:
                line_no = content[:m.start()].count("\n") + 1
                violations.append((fpath, line_no, f"Over-explained Doxygen prose paragraph ({max_in_block} consecutive lines)"))
    return violations


def check_include_placement_and_ordering():
    """
    Check 28: Enforces that project headers ("...") precede system headers (<...>),
    and no #include directives appear after C++ declarations/code.
    """
    violations = []
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        lines = content.splitlines()
        has_seen_angled = False
        has_seen_code = False
        in_comment = False
        for idx, line in enumerate(lines, 1):
            stripped = line.strip()
            if in_comment:
                if "*/" in stripped:
                    in_comment = False
                continue
            if stripped.startswith("/*"):
                if "*/" not in stripped:
                    in_comment = True
                continue
            if not stripped or stripped.startswith("//") or stripped.startswith("*"):
                continue
            if stripped.startswith("#"):
                if stripped.startswith("#include"):
                    if has_seen_code:
                        violations.append((fpath, idx, f"Include after code: {stripped}"))
                    if "<" in stripped:
                        has_seen_angled = True
                    elif '"' in stripped and has_seen_angled:
                        violations.append((fpath, idx, f"Quoted include after angled include: {stripped}"))
                continue
            has_seen_code = True
    return violations


def check_no_forward_declarations():
    """
    Check 29: Enforces Zero Forward Declarations across server/src/ (PROHIBITION 15).
    """
    violations = []
    fwd_re = re.compile(r"^\s*(?:class|struct)\s+([A-Za-z0-9_]+)\s*;")
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        no_comments = re.sub(r"//.*", "", content)
        no_comments = re.sub(r"/\*.*?\*/", "", no_comments, flags=re.DOTALL)
        for idx, line in enumerate(no_comments.splitlines(), 1):
            m = fwd_re.match(line)
            if m and "friend" not in line:
                violations.append((fpath, idx, line.strip()))
    return violations


def check_no_ghost_aliases():
    """
    Check 30: Enforces Zero Ghost Type Alias Shims (PROHIBITION 15).
    """
    violations = []
    alias_patterns = [
        re.compile(r"^\s*using\s+\w+Request\s*=\s*AstCheckContext\s*;"),
        re.compile(r"^\s*using\s+MemberAccess\s*=\s*MemberLookupResult\s*;"),
        re.compile(r"^\s*using\s+CallArgTypes\s*=\s*CallResolutionContext\s*;"),
        re.compile(r"^\s*using\s+ConversionTypes\s*=\s*ConversionTypePair\s*;"),
        re.compile(r"^\s*using\s+IncludeCandidate\s*=\s*TypoCandidate\s*;"),
        re.compile(r"^\s*using\s+TypoCandidateSuggestion\s*=\s*TypoCandidate\s*;"),
        re.compile(r"^\s*using\s+DocumentFormattingRequest\s*=\s*FormattingRequest\s*;"),
    ]
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            for pat in alias_patterns:
                if pat.search(line):
                    violations.append((fpath, idx, line.strip()))
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

    for path, line_number, line in check_indexed_checkers_use_facts():
        has_errors = True
        print(f"[ERROR] {path.relative_to(REPO_ROOT)}:{line_number}: use indexed facts for navigation: {line}")

    for path, line_number, line in check_single_syntax_producer():
        has_errors = True
        print(f"[ERROR] {path.relative_to(REPO_ROOT)}:{line_number}: use ResolveSyntaxRevision: {line}")

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

    parser_nodes_violations = check_no_parser_nodes_in_layers_2_3_4()
    if parser_nodes_violations:
        has_errors = True
        print("[ERROR] parser::nodes:: detected in Layer 2/3/4 (use gs.sym* from parser::GetGrammarSymbols()):")
        for f, line_num, line in parser_nodes_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    cursor_field_violations = check_no_cursor_field_name()
    if cursor_field_violations:
        has_errors = True
        print("[ERROR] ts_tree_cursor_current_field_name detected in Layer 2/3/4 (use ts_tree_cursor_current_field_id):")
        for f, line_num, line in cursor_field_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    depth_violations = check_no_depth_number_comparisons()
    if depth_violations:
        has_errors = True
        print("[ERROR] Magic depth comparisons or k_maxAstDepth detected in Layer 2/3/4 (use flat iterative worklists):")
        for f, line_num, line in depth_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    duplicate_to_source_range_violations = check_no_duplicate_to_source_range()
    if duplicate_to_source_range_violations:
        has_errors = True
        print("[ERROR] Duplicate ToSourceRange definition detected in .cpp files (use centralized SymbolTable.h ToSourceRange):")
        for f, line_num, line in duplicate_to_source_range_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    descendant_violations = check_no_for_each_descendant_node()
    if descendant_violations:
        has_errors = True
        print("[ERROR] ForEachDescendantNode detected in server/src/ (eradicated in favor of NodeIndex and flat cursors):")
        for f, line_num, line in descendant_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    analysis_ast_utils_violations = check_no_analysis_ast_utils()
    if analysis_ast_utils_violations:
        has_errors = True
        print("[ERROR] analysis/ASTUtils.h still exists! It must be unified into parser/ASTUtils.h:")
        for f in analysis_ast_utils_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}")

    banned_id_violations = check_no_banned_identifiers()
    if banned_id_violations:
        has_errors = True
        print("[ERROR] Banned identifier or function detected in server/src/ (const-laundering eradication):")
        for f, line_num, line, reason in banned_id_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line} -> {reason}")

    test_baseclass_violations = check_no_baseclass_in_tests()
    if test_baseclass_violations:
        has_errors = True
        print("[ERROR] Literal 'BaseClass' detected in server/tests/ outside GenerateRandomSymbolName('BaseClass'):")
        for f, line_num, line in test_baseclass_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    qualifier_violations = check_no_manual_type_qualifier_parsing()
    if qualifier_violations:
        has_errors = True
        print("[ERROR] Manual type qualifier reparsing detected in Layer 2 or Layer 3 (use precalculated Pass-1 flags or TypeSanitization.h):")
        for f, line_num, line, reason in qualifier_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line} -> {reason}")

    anon_violations = check_no_anonymous_namespaces()
    if anon_violations:
        has_errors = True
        print("[ERROR] Anonymous namespace(s) detected in server/src/ (use file-scope static or <module>_detail):")
        for f, line_num, line in anon_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    using_violations = check_no_using_directives_or_declarations()
    if using_violations:
        has_errors = True
        print("[ERROR] 'using namespace' or 'using <ns>::<symbol>;' detected in server/src/ (fully qualify namespaces or use type aliases):")
        for f, line_num, line in using_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    slop_violations = check_no_doxygen_slop()
    if slop_violations:
        has_errors = True
        print("[ERROR] Mechanical/robotic Doxygen slop template detected in server/src/:")
        for f, line_num, line, reason in slop_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line} -> {reason}")

    detail_ns_violations = check_no_detail_namespaces()
    if detail_ns_violations:
        has_errors = True
        print("[ERROR] namespace *_detail detected in server/src/ (prohibited, all structs must reside in canonical headers or private class scopes):")
        for f, line_num, line in detail_ns_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    detail_using_violations = check_no_detail_using_aliases()
    if detail_using_violations:
        has_errors = True
        print("[ERROR] using alias for *_detail detected in server/src/ (prohibited):")
        for f, line_num, line in detail_using_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    cpp_struct_violations = check_no_cpp_struct_declarations()
    if cpp_struct_violations:
        has_errors = True
        print("[ERROR] Direct struct declaration detected in .cpp in Layer 2 or Layer 3 (prohibited, move to canonical header):")
        for f, line_num, line, sname in cpp_struct_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: struct '{sname}'")

    odr_safety = check_cpp_structs_odr_safety()
    if odr_safety["cross_collisions"] or odr_safety["header_collisions"]:
        has_errors = True
        if odr_safety["cross_collisions"]:
            print("[ERROR] Struct/class name collision between different .cpp files (ODR violation risk):")
            for name, locs in odr_safety["cross_collisions"]:
                print(f"  Struct '{name}' duplicated in:")
                for f, line_num in locs:
                    rel = f.relative_to(REPO_ROOT)
                    print(f"    {rel}:{line_num}")
        if odr_safety["header_collisions"]:
            print("[ERROR] Struct/class in .cpp collides with header .h declaration (ODR violation risk):")
            for name, locs in odr_safety["header_collisions"]:
                print(f"  Struct '{name}' also exists in a header, declared in:")
                for f, line_num in locs:
                    rel = f.relative_to(REPO_ROOT)
                    print(f"    {rel}:{line_num}")

    stacked_doxygen_violations = check_no_stacked_doxygen_blocks()
    if stacked_doxygen_violations:
        has_errors = True
        print("[ERROR] Stacked Doxygen comment blocks (*/ ... /**) detected in server/src/:")
        for f, line_num, desc in stacked_doxygen_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {desc}")

    banner_violations = check_no_decorative_comment_banners()
    if banner_violations:
        has_errors = True
        print("[ERROR] Decorative comment divider/banner detected in server/src/:")
        for f, line_num, line in banner_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    narration_violations = check_no_workflow_narration_comments()
    if narration_violations:
        has_errors = True
        print("[ERROR] Workflow narration comment (// Step 1, // First,, // Finally,) detected in server/src/:")
        for f, line_num, line in narration_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    prose_violations = check_no_over_explained_doxygen_prose()
    if prose_violations:
        has_errors = True
        print("[ERROR] Over-explained Doxygen prose paragraph detected in server/src/ (concise ceiling exceeded):")
        for f, line_num, desc in prose_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {desc}")

    include_violations = check_include_placement_and_ordering()
    if include_violations:
        has_errors = True
        print("[ERROR] Include ordering/placement violation detected in server/src/ (project headers before system headers, no include after code):")
        for f, line_num, desc in include_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {desc}")

    fwd_violations = check_no_forward_declarations()
    if fwd_violations:
        has_errors = True
        print("[ERROR] Forward declaration detected in server/src/ (PROHIBITION 15, include canonical header):")
        for f, line_num, line in fwd_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    ghost_violations = check_no_ghost_aliases()
    if ghost_violations:
        has_errors = True
        print("[ERROR] Ghost type alias shim detected in server/src/ (PROHIBITION 15, use canonical type directly):")
        for f, line_num, line in ghost_violations:
            rel = f.relative_to(REPO_ROOT)
            print(f"  {rel}:{line_num}: {line}")

    if has_errors:
        sys.exit(1)

    print("[PASSED] AST Traversal, Parent Isolation, Agnosticism, Type & Scope Invariants verified (0 root-climb loops, 0 raw ts_node_parent calls, 0 ts_node_type in layers 2/3, 0 NodeType in layers 2/3/4, 0 modifier string literals in analysis, 0 parser::fields:: in layers 2/3, 0 manual scope finds, 0 op NodeText, 0 kw fallbacks, 0 scope concatenations in rules, 0 parser::nodes:: in layers 2/3/4, 0 ts_tree_cursor_current_field_name in layers 2/3/4, 0 k_maxAstDepth, 0 duplicate ToSourceRange, 0 ForEachDescendantNode, 0 engine hardcodings, 0 type hardcodings, 0 banned identifiers, 0 unrandomized test BaseClass, 0 manual qualifier reparsing, 0 anonymous namespaces, 0 using directives, 0 doxygen slop templates, 0 ODR struct violations, 0 stacked doxygen blocks, 0 decorative banners, 0 workflow narration comments, 0 over-explained doxygen essays, 0 include ordering violations, 0 forward declarations, 0 ghost aliases, 0 direct navigation calls in indexed checkers, 0 direct LSP syntax producers outside ResolveSyntaxRevision).")

if __name__ == "__main__":
    main()

