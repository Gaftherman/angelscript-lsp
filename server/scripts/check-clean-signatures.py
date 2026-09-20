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

def strip_string_literals(content: str) -> str:
    """Replaces contents of string literals with spaces while preserving newlines and length."""
    def replace_raw(m):
        nl = m.group(0).count('\n')
        return '""' + '\n' * nl + ' ' * (len(m.group(0)) - 2 - nl)
    def replace_str(m):
        nl = m.group(0).count('\n')
        return '""' + '\n' * nl + ' ' * (len(m.group(0)) - 2 - nl)
    
    content = re.sub(r'R"([a-zA-Z0-9_]*)\(.*?\)\1"', replace_raw, content, flags=re.DOTALL)
    content = re.sub(r'"(?:\\.|[^"\\])*"', replace_str, content)
    return content

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

# Pattern 2: Explicit unnamed parameters in function definitions
UNNAMED_PARAM = re.compile(
    r'\b(int|bool|float|double|uint32_t|uint64_t|size_t|std::string|std::string_view|TSNode|Position|Document|SymbolTable)\s*(?:const\s*)?[*&]?\s*([,)])'
)

def audit_file(path: Path):
    rel_path = path.relative_to(ROOT)
    raw_content = path.read_text(encoding="utf-8", errors="replace")
    content = strip_string_literals(raw_content)
    
    # 1. Detect commented-out parameter names or blocks
    for match in COMMENT_IN_PARAMS.finditer(content):
        matched_str = match.group(0)
        if not re.search(r'/\*!\s*|/\*<\s*', matched_str):
            line_no = content[:match.start()].count('\n') + 1
            VIOLATIONS.append(
                f"{rel_path}:{line_no} -> Commented parameter trick detected: '{matched_str.strip()}'. "
                "Delete the parameter completely and refactor all callers."
            )

    # 2. Detect unnamed parameters in .cpp files (excluding operators)
    if path.suffix == ".cpp" and "operator" not in path.name:
        for match in UNNAMED_PARAM.finditer(content):
            if is_in_template_brackets(content, match.start()):
                continue
            line_no = content[:match.start()].count('\n') + 1
            line = raw_content.splitlines()[line_no - 1].strip()
            if line.startswith("//") or line.startswith("*") or line.startswith("/*"):
                continue
            snippet = content[max(0, match.start() - 30):min(len(content), match.end() + 30)]
            if "template" not in snippet and "static_cast" not in snippet:
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
