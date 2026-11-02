#!/usr/bin/env python3
"""
Refactor Ledger Integrity & Traceability Quality Gate.
Enforces:
1. docs/REFACTOR_LEDGER.json conforms strictly to the schema.
2. Every entry contains all mandatory fields and valid values.
3. Every replacedByCanonicalFile exists and contains replacedByCanonicalSymbol.
4. If --check-diff <base_commit> is given, verifies that any deleted struct or
   function in git diff has a corresponding ledger entry.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
LEDGER_PATH = REPO_ROOT / "docs" / "REFACTOR_LEDGER.json"

VALID_PHASES = {"R0", "R1", "R2", "R3", "R4"}
VALID_ACTIONS = {
    "UNIFY_FUNCTIONS",
    "UNIFY_STRUCT",
    "DELETE_REDUNDANT_FLAG_CHECK",
    "COMPACT_RAM_LAYOUT",
    "CLEAN_SENTINEL",
}

REQUIRED_ENTRY_FIELDS = [
    "id",
    "phase",
    "action",
    "sourceFile",
    "deletedLineRange",
    "originalSymbolOrCode",
    "whatItDidAndWhyInThatFile",
    "whyChangedOrDeleted",
    "replacedByCanonicalFile",
    "replacedByCanonicalSymbol",
    "ramOrCpuImpact",
    "verifiedByTests",
]


def validate_ledger_schema():
    violations = []
    if not LEDGER_PATH.is_file():
        return [f"Missing ledger file: {LEDGER_PATH}"]

    try:
        data = json.loads(LEDGER_PATH.read_text(encoding="utf-8"))
    except Exception as exc:
        return [f"Malformed JSON in {LEDGER_PATH}: {exc}"]

    if data.get("schemaVersion") != "1.0":
        violations.append(f"Expected schemaVersion '1.0', got '{data.get('schemaVersion')}'")
    if not data.get("branch"):
        violations.append("Missing or empty 'branch' in ledger root")
    if not isinstance(data.get("entries"), list):
        violations.append("Root property 'entries' must be a list")
        return violations

    seen_ids = set()
    for idx, entry in enumerate(data["entries"]):
        prefix = f"Entry [{idx}]"
        if not isinstance(entry, dict):
            violations.append(f"{prefix} is not a JSON object")
            continue

        entry_id = entry.get("id")
        if not entry_id or not isinstance(entry_id, str):
            violations.append(f"{prefix} missing or invalid 'id'")
        else:
            prefix = f"Entry '{entry_id}'"
            if entry_id in seen_ids:
                violations.append(f"{prefix} duplicate ID found")
            seen_ids.add(entry_id)

        for req in REQUIRED_ENTRY_FIELDS:
            val = entry.get(req)
            if val is None or (isinstance(val, str) and not val.strip()):
                violations.append(f"{prefix} has empty or missing mandatory field '{req}'")

        phase = entry.get("phase")
        if phase not in VALID_PHASES:
            violations.append(f"{prefix} invalid phase '{phase}', expected one of {sorted(VALID_PHASES)}")

        action = entry.get("action")
        if action not in VALID_ACTIONS:
            violations.append(f"{prefix} invalid action '{action}', expected one of {sorted(VALID_ACTIONS)}")

        canon_file_rel = entry.get("replacedByCanonicalFile")
        canon_sym = entry.get("replacedByCanonicalSymbol")

        if canon_file_rel and isinstance(canon_file_rel, str) and canon_file_rel != "N/A":
            canon_file = REPO_ROOT / canon_file_rel.replace("/", "\\")
            if not canon_file.is_file():
                # Try forward-slash as well on POSIX/Windows Path
                canon_file = REPO_ROOT / canon_file_rel
            if not canon_file.is_file():
                violations.append(f"{prefix} canonical file '{canon_file_rel}' does not exist on disk")
            elif canon_sym and isinstance(canon_sym, str) and canon_sym != "N/A":
                try:
                    content = canon_file.read_text(encoding="utf-8", errors="ignore")
                    if canon_sym not in content:
                        violations.append(
                            f"{prefix} canonical symbol '{canon_sym}' not found in '{canon_file_rel}'"
                        )
                except Exception as exc:
                    violations.append(f"{prefix} failed to read '{canon_file_rel}': {exc}")

        tests = entry.get("verifiedByTests")
        if not isinstance(tests, list) or len(tests) == 0:
            violations.append(f"{prefix} 'verifiedByTests' must be a non-empty list of test names")

    return violations


def is_sym_declared_in_file_or_peer(sym: str, rel_path: str) -> bool:
    if not rel_path:
        return False
    target_path = REPO_ROOT / rel_path.replace("/", "\\") if sys.platform == "win32" else REPO_ROOT / rel_path
    if not target_path.is_file():
        target_path = REPO_ROOT / rel_path
    if not target_path.is_file():
        return False

    candidates = [target_path]
    if target_path.suffix == ".cpp":
        candidates.append(target_path.with_suffix(".h"))
    elif target_path.suffix == ".h":
        candidates.append(target_path.with_suffix(".cpp"))

    func_pattern = re.compile(rf"(?:^|\n)\s*(?:[A-Za-z0-9_:<>&*\[\]]+\s+)+{re.escape(sym)}\s*\(")
    struct_pattern = re.compile(rf"\b(?:struct|class)\s+{re.escape(sym)}\b")

    for cand in candidates:
        if cand.is_file():
            try:
                content = cand.read_text(encoding="utf-8", errors="ignore")
                if func_pattern.search(content) or struct_pattern.search(content):
                    return True
            except Exception:
                pass
    return False


def is_symbol_recorded(sym: str, recorded_symbols: set) -> bool:
    return any(re.search(rf"\b{re.escape(sym)}\b", s) for s in recorded_symbols)


def check_git_diff(base_commit: str):
    violations = []
    try:
        ref = base_commit
        chk = subprocess.run(
            ["git", "rev-parse", "--verify", ref],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        if chk.returncode != 0:
            chk_origin = subprocess.run(
                ["git", "rev-parse", "--verify", f"origin/{ref}"],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )
            if chk_origin.returncode == 0:
                ref = f"origin/{ref}"

        diff_res = subprocess.run(
            ["git", "diff", f"{ref}..HEAD", "-U0", "--", "server/src/"],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            check=True,
        )
    except Exception as exc:
        return [f"Failed to run git diff: {exc}"]

    ledger_data = json.loads(LEDGER_PATH.read_text(encoding="utf-8"))
    recorded_symbols = {e.get("originalSymbolOrCode", "") for e in ledger_data.get("entries", [])}

    struct_del_re = re.compile(r"^-\s*(?:struct|class)\s+([A-Za-z0-9_]+)\b")
    func_del_re = re.compile(r"^-\s*(?:[A-Za-z0-9_:<>&*\[\]]+\s+)+([A-Za-z0-9_]+)\s*\(")
    var_init_re = re.compile(r"^-\s*(?:[A-Za-z0-9_:<>&*\[\]]+\s+)+[A-Za-z0-9_]+\s*\(\s*[A-Za-z0-9_]+\s*\)\s*;")
    ignored_keywords = {
        "if",
        "for",
        "while",
        "switch",
        "catch",
        "return",
        "throw",
        "sizeof",
        "decltype",
        "static_assert",
        "case",
        "default",
        "operator",
    }

    curr_file = ""
    in_func_block = False

    for line in diff_res.stdout.splitlines():
        if line.startswith("diff --git "):
            parts = line.split()
            if len(parts) >= 4:
                b_path = parts[3]
                if b_path.startswith("b/"):
                    b_path = b_path[2:]
                curr_file = b_path
            in_func_block = False
            continue
        if line.startswith("+++ "):
            target = line[4:].strip()
            if target.startswith("b/"):
                target = target[2:]
            if target != "/dev/null":
                curr_file = target
            in_func_block = False
            continue
        if line.startswith("@@"):
            match_hunk = re.search(r"@@\s+(.*)$", line)
            if match_hunk:
                ctx = match_hunk.group(1).strip()
                if "(" in ctx and ")" in ctx and not ctx.startswith(("struct ", "class ")):
                    in_func_block = True
                else:
                    in_func_block = False
            else:
                in_func_block = False
            continue

        if not line.startswith("-") or line.startswith("---"):
            continue

        # Skip comments and member initializer list entries
        if re.match(r"^-\s*(?://|/\*|\*)", line) or re.match(r"^-\s*[:,]", line):
            continue

        # Skip statements that cannot be function or method declarations
        if re.match(r"^-\s*(?:return|co_return|throw|goto|delete|new|break|continue)\b", line):
            continue

        # 1. Struct and class deletions: never skipped by indentation!
        m_struct = struct_del_re.match(line)
        if m_struct:
            sym = m_struct.group(1)
            if not is_symbol_recorded(sym, recorded_symbols) and not is_sym_declared_in_file_or_peer(sym, curr_file):
                violations.append(f"Deleted struct '{sym}' not recorded in REFACTOR_LEDGER.json")
            continue

        # Skip local statements/variables indented inside function implementations
        if in_func_block and re.match(r"^-(?:\s{4,}|\t)", line):
            continue

        # Skip direct-initialization local variables (e.g. `Type var(arg);`)
        if var_init_re.match(line):
            continue

        # 2. Function and method declarations / definitions
        m_func = func_del_re.match(line)
        if m_func:
            sym = m_func.group(1)
            if sym not in ignored_keywords and not is_symbol_recorded(sym, recorded_symbols):
                if not is_sym_declared_in_file_or_peer(sym, curr_file):
                    violations.append(f"Deleted function '{sym}' not recorded in REFACTOR_LEDGER.json")

    return violations


def main():
    parser = argparse.ArgumentParser(description="Check integrity of docs/REFACTOR_LEDGER.json")
    parser.add_argument("--check-diff", type=str, default="", help="Base commit to verify deleted symbols against")
    args = parser.parse_args()

    violations = validate_ledger_schema()
    if args.check_diff:
        violations.extend(check_git_diff(args.check_diff))

    if violations:
        print(f"[FAILED] {len(violations)} refactor ledger violations detected:")
        for v in violations:
            print(f"  - {v}")
        sys.exit(1)

    print("[PASSED] docs/REFACTOR_LEDGER.json passed all structural and canonical integrity checks.")
    sys.exit(0)


if __name__ == "__main__":
    main()
