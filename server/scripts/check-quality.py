#!/usr/bin/env python3
import subprocess
import sys
import shutil
from pathlib import Path

# Repository root (parent of 'server')
REPO_ROOT = Path(__file__).resolve().parent.parent.parent

passed_steps = []
skipped_steps = []

def run_step(name, command):
    print(f"\n[RUNNING] {name}...")
    res = subprocess.run(command, shell=True, cwd=REPO_ROOT)
    if res.returncode != 0:
        print(f"[FAILED] {name} detected quality violations (Exit code: {res.returncode}).")
        sys.exit(res.returncode)
    print(f"[PASSED] {name}")
    passed_steps.append(name)

def skip_step(name, reason):
    print(f"\n[SKIPPED] {name} ({reason})")
    skipped_steps.append((name, reason))

def main():
    # 1. Architectural Layer Matrix
    run_step("Layer Architecture Invariants", "python server/scripts/check-layer-includes.py")

    # 2. Dead Parameter & Clean Signature Enforcement
    run_step("Clean Function Signatures", "python server/scripts/check-clean-signatures.py")

    # 3. Diagnostic Code Registration Integrity
    run_step("Diagnostic Codes Registry", "python server/scripts/check-diagnostic-codes.py")

    # 4. Client Localization Completeness (English & Spanish)
    run_step("Client Localization Integrity", "python server/scripts/check-client-l10n.py")
    run_step("README Settings Catalog Sync", "python server/scripts/sync-readme-settings.py --check")

    # 5. Tree-Sitter Grammar Names & Pin Audit
    run_step("Tree-Sitter Grammar Names", "python server/scripts/check-grammar-names.py")
    run_step("Tree-Sitter Grammar Pin", "python server/scripts/check-grammar-pin.py")

    # 6. Code Duplication Gate (jscpd threshold <= 3%)
    if shutil.which("npx"):
        run_step("Code Duplication (jscpd)", "npx jscpd server/src/")
    else:
        skip_step("Code Duplication (jscpd)", "npx not found in PATH")

    # 7. Cyclomatic & Cognitive Complexity (Lizard)
    run_step("Cyclomatic Complexity (Lizard)", "python -m lizard server/src/ -C 15 -L 70 -w")

    # 8. AST Antipattern & Memory Allocation Gate (ast-grep)
    if shutil.which("ast-grep"):
        run_step("AST Antipattern Gate (ast-grep)", "ast-grep scan server/src/")
    elif shutil.which("sg"):
        run_step("AST Antipattern Gate (sg)", "sg scan server/src/")
    else:
        skip_step("AST Antipattern Gate", "ast-grep/sg not found in PATH")

    # 9. Deep Static Analysis (Cppcheck)
    if shutil.which("cppcheck"):
        run_step(
            "Static Analysis (Cppcheck)",
            "cppcheck --enable=warning,performance,portability,style "
            "--error-exitcode=1 --suppress=missingInclude --suppress=unusedFunction "
            "--inline-suppr --std=c++20 server/src/"
        )
    else:
        skip_step("Static Analysis (Cppcheck)", "cppcheck not found in PATH")

    # 10. Clang-Format Code Style Verification
    if shutil.which("clang-format"):
        run_step("Clang-Format Style Check", "python server/scripts/format-code.py --check")
    else:
        skip_step("Clang-Format Style Check", "clang-format not found in PATH")

    print("\n=======================================================")
    print(f"[SUMMARY] {len(passed_steps)} gates passed, {len(skipped_steps)} gates skipped.")
    if skipped_steps:
        print("Skipped gates:")
        for name, reason in skipped_steps:
            print(f"  - {name}: {reason}")
    print("=======================================================")
    print("[SUCCESS] All active static quality gates passed successfully.")

if __name__ == "__main__":
    main()
