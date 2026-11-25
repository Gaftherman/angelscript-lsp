#!/usr/bin/env python3
"""Check second-pass AST boundaries and optionally flag avoidable string copies.

Scan SECOND_PASS_FOLDERS, the shared utilities, and any additional paths.
Reject Tree-Sitter symbols and calls, forbidden headers, and raw AST access.
In strict string-view mode, reject avoidable std::string copies. The ownership
audit counts std::string uses without failing on the count.

String checks exempt utilities and the modules that own source text (document),
parsed CLI/JSON settings (config), formatted messages (i18n), diagnostic messages
and URIs (Diagnostics.h), Markdown for hover, signatures, and inlay hints
(documentation), and type caches, profiles, converted config, and sanitized
names (types). Second-pass views borrow the document text, and i18n lookup
already uses string views. The AST checks still apply in these locations.
"""

import argparse
import re
import sys
from pathlib import Path

FIRST_PASS_FOLDERS = {
    "server/src/parser",
    "server/src/analysis/index",
}

SECOND_PASS_FOLDERS = (
    "server/src/analysis/completion",
    "server/src/analysis/code_action",
    "server/src/analysis/formatting",
    "server/src/features/selection_range",
    "server/src/features/linked_editing",
    "server/src/features/folding_range",
    "server/src/features/document_link",
    "server/src/features/workspace_symbol",
    "server/src/features/document_symbol",
    "server/src/features/type_hierarchy",
    "server/src/features/call_hierarchy",
    "server/src/features/implementation",
    "server/src/features/breakpoints",
    "server/src/features/inlay_hint",
    "server/src/features/definition",
    "server/src/features/references",
    "server/src/features/rename",
    "server/src/features/document_highlight",
    "server/src/features/signature_help",
    "server/src/features/code_lens",
    "server/src/features/semantic_tokens",
    "server/src/features/hover",
    "server/src/features/formatting",
    "server/src/features/completion",
    "server/src/features/code_action",
    "server/src/document",
    "server/src/i18n",
    "server/src/config",
    "server/src/utils",
    "server/src/analysis/diagnostics",
    "server/src/analysis/documentation",
    "server/src/analysis/types",
    "server/src/analysis/mediator",
    "server/src/parser",
    "server/src/analysis/rules",
    "server/src/analysis/checkers/null_safety",
    "server/src/analysis/checkers",
    "server/src/analysis/resolution",
    "server/src/lsp/state",
    "server/src/lsp/services",
    "server/src/lsp/pipeline",
    "server/src/lsp/handlers",
    "server/src/lsp",
)

STRING_OWNER_FOLDERS = (
    "server/src/utils/",
    "server/src/document/",
    "server/src/i18n/",
    "server/src/config/",
    "server/src/analysis/diagnostics/Diagnostics.h",
    "server/src/analysis/documentation/",
    "server/src/analysis/types/",
)

ALWAYS_BLOCKED_UTILS = [
    "server/src/utils",
    "server/src/features/common",
]

BANNED_TS_REGEX = re.compile(
    r"\b(TSNode|TSTree|TSTreeCursor|TSPoint|TSInputEdit|ts_tree_[a-z0-9_]+|ts_node_[a-z0-9_]+|"
    r"ASTUtils\.h|tree_sitter/api\.h|FindAncestorWithSymbol|GetParent)\b|->Root\s*\("
)

BANNED_STD_STRING_REGEX = re.compile(r"\bstd::string\b")

AVOIDABLE_COPY_PATTERNS = (
    (
        re.compile(r"[(,]\s*const\s+std::string\s*&\s*\w*\s*[,)]"),
        "read-only 'const std::string&' parameter (use std::string_view)",
    ),
    (
        re.compile(r"\.(find|contains|count|erase|at)\(\s*std::string\("),
        "lookup with a temporary std::string (use a transparent map or set and the view)",
    ),
    (
        re.compile(r"\bstd::string\([^;]*?\)\s*\.(starts_with|ends_with|find|compare|substr|empty|size)\b"),
        "std::string copy used only for inspection (operate on the view)",
    ),
)


def find_avoidable_copy(clean: str) -> str | None:
    """Return why a line holds an avoidable string copy, or None when it is acceptable ownership."""
    if re.match(r"\s*for\s*\(", clean):
        return None
    for pattern, reason in AVOIDABLE_COPY_PATTERNS:
        if pattern.search(clean):
            return reason
    return None


def strip_comments(line: str) -> str:
    idx = line.find("//")
    if idx != -1:
        line = line[:idx]
    return line.strip()


def collect_files_to_check(root: Path, extra_paths: list[str]) -> list[Path]:
    folders = set(ALWAYS_BLOCKED_UTILS)
    folders.update(folder for folder in SECOND_PASS_FOLDERS if folder not in FIRST_PASS_FOLDERS)

    for extra in extra_paths:
        if extra not in FIRST_PASS_FOLDERS:
            folders.add(extra)

    files: list[Path] = []
    for folder in sorted(folders):
        folder_path = root / folder
        if folder_path.is_file():
            files.append(folder_path)
        elif folder_path.is_dir():
            files.extend(sorted(folder_path.rglob("*.h")))
            files.extend(sorted(folder_path.rglob("*.cpp")))
    return files


def main() -> None:
    parser = argparse.ArgumentParser(description="Check second-pass AST boundaries and avoidable string copies")
    parser.add_argument("--extra", nargs="*", default=[], help="Additional files or directories to check")
    parser.add_argument(
        "--strict-string-view",
        action="store_true",
        help="Reject avoidable second-pass std::string copies (const& parameters, temporary lookup keys, inspection copies)",
    )
    parser.add_argument(
        "--audit-string-ownership",
        action="store_true",
        help="Report total std::string use (including owning storage) without failing on the count",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    files = collect_files_to_check(root, args.extra)
    violations: list[str] = []
    ownership_hits = 0

    for file_path in files:
        rel = file_path.relative_to(root).as_posix()
        lines = file_path.read_text(encoding="utf-8", errors="replace").splitlines()
        in_block_comment = False

        for lineno, raw_line in enumerate(lines, 1):
            line = raw_line.strip()
            if "/*" in line:
                in_block_comment = True
            if in_block_comment:
                if "*/" in line:
                    in_block_comment = False
                continue

            clean = strip_comments(raw_line)
            if not clean:
                continue

            m_ts = BANNED_TS_REGEX.search(clean)
            if m_ts:
                violations.append(
                    f"[VIOLATION AST/TS_*] {rel}:{lineno} -> '{m_ts.group(0)}' found in second-pass or utility code:\n    {clean}"
                )

            if (args.strict_string_view or args.audit_string_ownership) and not rel.startswith(STRING_OWNER_FOLDERS):
                if args.audit_string_ownership and BANNED_STD_STRING_REGEX.search(clean):
                    ownership_hits += 1
                reason = find_avoidable_copy(clean) if args.strict_string_view else None
                if reason:
                    violations.append(f"[VIOLATION STD::STRING] {rel}:{lineno} -> {reason}:\n    {clean}")

    if args.audit_string_ownership:
        print(f"[AUDIT] {ownership_hits} lines use std::string (including permitted owning storage).")

    if violations:
        print("\n".join(violations))
        sys.exit(f"\n[FAIL] Found {len(violations)} violations in the scanned folders.")

    print(f"[PASS] {len(files)} scanned files passed the AST and enabled second-pass checks.")


if __name__ == "__main__":
    main()
