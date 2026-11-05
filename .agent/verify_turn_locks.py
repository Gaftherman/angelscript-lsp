#!/usr/bin/env python3
"""
Validador automatizado de los 4 Candados Arquitectónicos por Turno:
- Candado A: Anti-omisión (exige diff real de código en todos los archivos del scope).
- Candado B: Anti-structs basura (bloquea nuevos struct/class fuera de REFACTOR_LEDGER.json).
- Candado C: Anti-delegación ciega (valida estructura JSON real en .agent/mcp_trace.json).
- Candado Lizard + Zero-Trust: Ejecuta lizard (-C 25 -L 120 -a 5 -w) y protege configs/reglas.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

PROTECTED_PATHS = [
    ".clang-tidy",
    ".clang-format",
    "server/CMakeLists.txt",
    "server/scripts",
    "server/rules",
    "docs/REFACTOR_LEDGER.json",
]

BANNED_ADHOC_STRUCTS = {
    "VirtualUriParts",
    "RecoveredAssignmentVar",
    "ReferenceContext",
}


def run_cmd(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True, encoding="utf-8", errors="replace")


def check_zero_trust(root: Path) -> None:
    res = run_cmd(["git", "diff", "--exit-code", "--"] + PROTECTED_PATHS, root)
    if res.returncode != 0:
        sys.exit(f"[FAIL - ZERO-TRUST GATE] Archivos protegidos fueron modificados:\n{res.stdout}")
    print("[PASS] Zero-Trust Gate: 0 archivos protegidos alterados.")


def check_lock_a_required_diffs(root: Path, required_files: list[str], base_ref: str = "HEAD") -> None:
    for rel_path in required_files:
        res = run_cmd(["git", "diff", "-U0", base_ref, "--", rel_path], root)
        changed_code_lines = [
            line for line in res.stdout.splitlines()
            if (line.startswith("+") or line.startswith("-"))
            and not line.startswith("+++")
            and not line.startswith("---")
            and not line[1:].strip().startswith("//")
            and not line[1:].strip().startswith("*")
            and not line[1:].strip().startswith("/*")
            and len(line[1:].strip()) > 0
        ]
        if not changed_code_lines:
            sys.exit(
                f"[FAIL - CANDADO A (Anti-Omisión)] El archivo '{rel_path}' está en el alcance obligatorio "
                f"pero no tiene cambios reales de código (solo comentarios o 0 diff)."
            )
    print(f"[PASS] Candado A (Anti-Omisión): {len(required_files)} archivos con diff real de código verificado.")


def check_lock_b_no_adhoc_structs(root: Path, target_files: list[str], base_ref: str = "HEAD") -> None:
    ledger_path = root / "docs" / "REFACTOR_LEDGER.json"
    ledger_text = ledger_path.read_text(encoding="utf-8") if ledger_path.exists() else ""

    struct_regex = re.compile(r"\b(?:struct|class)\s+([A-Za-z_][A-Za-z0-9_]*)\b(?:\s*[:{])")

    for rel_path in target_files:
        full_path = root / rel_path
        if not full_path.exists():
            continue
        content = full_path.read_text(encoding="utf-8", errors="replace")
        for match in struct_regex.finditer(content):
            name = match.group(1)
            if name in BANNED_ADHOC_STRUCTS:
                sys.exit(
                    f"[FAIL - CANDADO B (Anti-Structs Basura)] Struct prohibido '{name}' aún presente en {rel_path}."
                )

        diff_res = run_cmd(["git", "diff", "-U0", base_ref, "--", rel_path], root)
        for line in diff_res.stdout.splitlines():
            if line.startswith("+") and not line.startswith("+++"):
                for match in struct_regex.finditer(line[1:]):
                    new_type = match.group(1)
                    if new_type not in ledger_text:
                        sys.exit(
                            f"[FAIL - CANDADO B (Anti-Structs Basura)] Nuevo struct/class ad-hoc '{new_type}' "
                            f"detectado en {rel_path} que no existe en docs/REFACTOR_LEDGER.json."
                        )
    print("[PASS] Candado B (Anti-Structs Basura): 0 structs ad-hoc o parameter-bags detectados.")


def check_lock_c_mcp_evidence(root: Path, trace_rel: str, min_queries: int) -> None:
    trace_path = root / trace_rel
    if not trace_path.exists():
        sys.exit(f"[FAIL - CANDADO C (Evidencia MCP)] No existe el archivo {trace_rel}.")

    try:
        data = json.loads(trace_path.read_text(encoding="utf-8"))
    except Exception as exc:
        sys.exit(f"[FAIL - CANDADO C (Evidencia MCP)] JSON inválido en {trace_rel}: {exc}")

    calls = data.get("mcp_calls", [])
    if not isinstance(calls, list) or len(calls) < min_queries:
        sys.exit(
            f"[FAIL - CANDADO C (Evidencia MCP)] Se requieren al menos {min_queries} llamadas MCP registradas, "
            f"encontradas: {len(calls) if isinstance(calls, list) else 0}."
        )

    required_servers = {"sequential-thinking", "clangd", "tree-sitter", "git"}
    seen_servers = set()
    for idx, entry in enumerate(calls):
        if not isinstance(entry, dict):
            sys.exit(f"[FAIL - CANDADO C] Entrada #{idx} no es un objeto JSON.")
        server = entry.get("server", "")
        tool = entry.get("tool", "")
        raw_response = entry.get("raw_response")
        if not server or not tool or not raw_response:
            sys.exit(f"[FAIL - CANDADO C] Entrada #{idx} incompleta (requiere server, tool, raw_response).")
        seen_servers.add(server)

    missing = required_servers - seen_servers
    if missing:
        sys.exit(f"[FAIL - CANDADO C (Evidencia MCP)] Faltan servidores MCP obligatorios en la traza: {missing}")

    print(f"[PASS] Candado C (Evidencia MCP): {len(calls)} llamadas estructuradas verificadas.")


def check_lizard_relaxed(root: Path, target_files: list[str]) -> None:
    res = run_cmd([sys.executable, "-m", "lizard"] + target_files + ["-C", "25", "-L", "120", "-a", "5", "-w"], root)
    if res.returncode != 0:
        sys.exit(f"[FAIL - LIZARD GATE (-C 25 -L 120 -a 5 -w)]:\n{res.stdout}\n{res.stderr}")
    print(f"[PASS] Lizard Gate (-C 25 -L 120 -a 5 -w): {len(target_files)} archivos 100% aprobados.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Validador de Candados Arquitectónicos")
    parser.add_argument("--trace", default=".agent/mcp_trace.json", help="Ruta al JSON de traza MCP")
    parser.add_argument("--min-queries", type=int, default=4, help="Mínimo de consultas MCP requeridas")
    parser.add_argument("--base-ref", default="HEAD", help="Base git ref for diff checks")
    parser.add_argument("--require-diff", nargs="+", required=True, help="Archivos que DEBEN tener diff real")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    check_zero_trust(root)
    check_lock_a_required_diffs(root, args.require_diff, args.base_ref)
    check_lock_b_no_adhoc_structs(root, args.require_diff, args.base_ref)
    check_lock_c_mcp_evidence(root, args.trace, args.min_queries)
    check_lizard_relaxed(root, args.require_diff)
    print("\n=== TODOS LOS CANDADOS ARQUITECTÓNICOS APROBADOS (PASS) ===")


if __name__ == "__main__":
    main()