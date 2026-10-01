#!/usr/bin/env python3
"""
Audits C++ source code to prevent the 'unnamed parameter' and 'commented parameter'
anti-patterns (e.g. `int /* b */`, `/* int b */`, or `void func(int a, int)`).
"""
import sys
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC_DIR = ROOT / "server" / "src"
VIOLATIONS = []

def strip_strings_and_comments(content: str) -> tuple[str, str]:
    """Replaces strings and comments with spaces while preserving newlines and length."""
    def replace_raw(m):
        nl = m.group(0).count('\n')
        return '""' + '\n' * nl + ' ' * (len(m.group(0)) - 2 - nl)
    def replace_str(m):
        nl = m.group(0).count('\n')
        return '""' + '\n' * nl + ' ' * (len(m.group(0)) - 2 - nl)
    def replace_comment(m):
        nl = m.group(0).count('\n')
        return '  ' + '\n' * nl + ' ' * (len(m.group(0)) - 2 - nl)

    # Strip raw strings and standard strings
    no_strings = re.sub(r'R"([a-zA-Z0-9_]*)\(.*?\)\1"', replace_raw, content, flags=re.DOTALL)
    no_strings = re.sub(r'"(?:\\.|[^"\\])*"', replace_str, no_strings)

    # Strip block and line comments from no_strings to form no_comments
    no_comments = re.sub(r'/\*.*?\*/', replace_comment, no_strings, flags=re.DOTALL)
    no_comments = re.sub(r'//[^\n]*', replace_comment, no_comments)

    return no_strings, no_comments

def is_in_template_brackets(content: str, pos: int) -> bool:
    """Checks if pos is enclosed inside C++ template angle brackets < ... >."""
    depth = 0
    i = pos - 1
    while i >= 0 and content[i] not in (';', '{', '}'):
        if content[i] == '>':
            depth += 1
        elif content[i] == '<':
            if depth > 0:
                depth -= 1
            else:
                return True
        i -= 1
    return False

# Pattern 1: Comment inside parameter parentheses (excluding Doxygen)
COMMENT_IN_PARAMS = re.compile(r'\(([^)\n]*?/\*.*?\*/[^)\n]*?)\)')

# Pattern 2: Explicit unnamed parameters in function definitions (expanded standard and domain types)
UNNAMED_PARAM = re.compile(
    r'\b(int|int8_t|int16_t|int32_t|int64_t|uint|uint8_t|uint16_t|uint32_t|uint64_t|size_t|float|double|bool|char|std::string|std::string_view|string_view|TSNode|TSTree|Position|Range|Location|Document|SymbolTable|TypeStore|NodeIndex|ScopeIndex|DocumentStore|WorkspaceIncludeGraph|DiagnosticSeverity)\s*(?:const\s*)?[*&]?\s*([,)])'
)

def audit_file(path: Path):
    rel_path = path.relative_to(ROOT)
    raw_content = path.read_text(encoding="utf-8", errors="replace")
    no_strings, no_comments = strip_strings_and_comments(raw_content)
    
    # 1. Detect commented-out parameter names or blocks
    for match in COMMENT_IN_PARAMS.finditer(no_strings):
        matched_str = match.group(0)
        if not re.search(r'/\*!\s*|/\*<\s*', matched_str):
            line_no = no_strings[:match.start()].count('\n') + 1
            VIOLATIONS.append(
                f"{rel_path}:{line_no} -> Commented parameter trick detected: '{matched_str.strip()}'. "
                "Delete the parameter completely and refactor all callers."
            )

    # 2. Detect unnamed parameters in .cpp files (excluding operators and alloc hooks)
    if path.suffix == ".cpp" and "operator" not in path.name:
        for match in UNNAMED_PARAM.finditer(no_comments):
            if is_in_template_brackets(no_comments, match.start()):
                continue
            snippet = no_comments[max(0, match.start() - 30):min(len(no_comments), match.end() + 30)]
            if "template" not in snippet and "static_cast" not in snippet:
                line_no = no_comments[:match.start()].count('\n') + 1
                VIOLATIONS.append(
                    f"{rel_path}:{line_no} -> Unnamed dead parameter near: '{match.group(0).strip()}'. "
                    "Remove the parameter from the header, implementation, and all call sites."
                )

if __name__ == "__main__":
    for ext in ["*.h", "*.cpp"]:
        for file in SRC_DIR.rglob(ext):
            audit_file(file)
            
    if VIOLATIONS:
        print("\n[!] SIGNATURE CLEANLINESS AUDIT FAILED:")
        for v in VIOLATIONS:
            print(f"  - {v}")
        print("\nFix: Cleanly delete obsolete parameters across the entire call graph.")
        sys.exit(1)
    else:
        print("[OK] Signatures audited. Zero commented or unnamed parameters found.")
