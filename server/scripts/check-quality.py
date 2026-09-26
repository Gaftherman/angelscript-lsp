#!/usr/bin/env python3
import subprocess
import sys
import shutil
from pathlib import Path

# Repository root (parent of 'server')
REPO_ROOT = Path(__file__).resolve().parent.parent.parent

def run_step(name, command):
    print(f"\n[RUNNING] {name}...")
    res = subprocess.run(command, shell=True, cwd=REPO_ROOT)
    if res.returncode != 0:
        print(f"[FAILED] {name} detected quality violations (Exit code: {res.returncode}).")
        sys.exit(res.returncode)
    print(f"[PASSED] {name}")

def main():
    # 1. Architectural Layer Matrix
    run_step("Layer Architecture Invariants", "python server/scripts/check-layer-includes.py")

    # 2. Dead Parameter & Clean Signature Enforcement
    run_step("Clean Function Signatures", "python server/scripts/check-clean-signatures.py")

    # 3. Diagnostic Code Registration Integrity
    run_step("Diagnostic Codes Registry", "python server/scripts/check-diagnostic-codes.py")

    # 4. Code Duplication Gate (jscpd threshold <= 3%)
    if shutil.which("npx"):
        run_step("Code Duplication (jscpd)", "npx jscpd server/src/")
    else:
        print("\n[SKIPPED] npx not found; skipping jscpd.")

    # 5. Cyclomatic & Cognitive Complexity (Lizard)
    run_step("Cyclomatic Complexity (Lizard)", "python -m lizard server/src/ -C 15 -L 70 -w")

    # 6. AST Antipattern & Memory Allocation Gate (ast-grep)
    if shutil.which("ast-grep"):
        run_step("AST Antipattern Gate (ast-grep)", "ast-grep scan server/src/")
    elif shutil.which("sg"):
        run_step("AST Antipattern Gate (sg)", "sg scan server/src/")
    else:
        print("\n[SKIPPED] ast-grep/sg not found in PATH.")

    # 7. Deep Static Analysis (Cppcheck)
    if shutil.which("cppcheck"):
        run_step(
            "Static Analysis (Cppcheck)",
            "cppcheck --enable=warning,performance,portability,style "
            "--error-exitcode=1 --suppress=missingInclude --suppress=unusedFunction "
            "--inline-suppr --std=c++20 server/src/"
        )
    else:
        print("\n[SKIPPED] cppcheck not found in PATH.")

    print("\n[SUCCESS] All static quality gates passed successfully.")

if __name__ == "__main__":
    main()
