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

VALID_PHASES = {"R0", "R1", "R2", "R3"}
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


def check_git_diff(base_commit: str):
    violations = []
    try:
        diff_res = subprocess.run(
            ["git", "diff", f"{base_commit}..HEAD", "-U0", "--", "server/src/"],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            check=True,
        )
    except Exception as exc:
        return [f"Failed to run git diff: {exc}"]

    ledger_data = json.loads(LEDGER_PATH.read_text(encoding="utf-8"))
    recorded_symbols = {e.get("originalSymbolOrCode", "") for e in ledger_data.get("entries", [])}

    struct_del_re = re.compile(r"^[-\s]*(?:struct|class)\s+(\w+)\s*\{", re.MULTILINE)
    func_del_re = re.compile(r"^-\s*(?:[A-Za-z0-9_:<>&*]+\s+)+([A-Za-z0-9_]+)\s*\([^)]*\)\s*\{?", re.MULTILINE)

    for line in diff_res.stdout.splitlines():
        if not line.startswith("-"):
            continue
        m_struct = struct_del_re.match(line)
        if m_struct:
            sym = m_struct.group(1)
            if not any(sym in s for s in recorded_symbols):
                violations.append(f"Deleted struct '{sym}' not recorded in REFACTOR_LEDGER.json")
        m_func = func_del_re.match(line)
        if m_func:
            sym = m_func.group(1)
            if sym not in ("if", "for", "while", "switch", "catch") and not any(sym in s for s in recorded_symbols):
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
