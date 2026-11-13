#!/usr/bin/env python3
"""
Validador Dinámico de Invariantes de Segunda Pasada (Zero-AST & Zero-std::string).
- Lee dinámicamente `completed_folders` y `active_folder` desde `docs/REFACTOR_STEPS.json`.
- Bloquea en todas las carpetas de 2da pasada ya verificadas (y en utils/common):
  1. Cualquier símbolo o función de Tree-Sitter (`ts_*`, `TSNode`, `TSTree`, `TSTreeCursor`, `TSPoint`).
  2. Inclusiones prohibidas (`ASTUtils.h`, `tree_sitter/api.h`) o escapes (`->Root()`, `ForEachNodeOrdered` con TSNode).
  3. Uso de `std::string` o `const std::string&` donde debe usarse `std::string_view` en 2da pasada.
- `utils/`, `document/`, `i18n/` y `config/` quedan exentos solo de la regla 3: `document/` es el dueño del texto fuente al que
  apuntan todas las vistas `std::string_view` de 2da pasada; `i18n/` devuelve mensajes formateados propios
  (`FormatMessage`) y su búsqueda ya es `std::string_view`; `config/` es dueño de los ajustes CLI/JSON parseados.
  `analysis/diagnostics/Diagnostics.h` es dueño del mensaje formateado y la URI de cada diagnóstico publicado.
  `analysis/documentation/` es dueño del Markdown que construye para hover, firmas e inlay hints.
  `analysis/types/` es dueño de la caché de tipos, el texto de perfiles, la configuración convertida y los nombres saneados.
  La regla 1 (cero ts_*) sigue aplicando.
"""

import argparse
import json
import re
import sys
from pathlib import Path

FIRST_PASS_FOLDERS = {
    "server/src/parser",
    "server/src/analysis/index",
}

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

BANNED_STD_STRING_REGEX = re.compile(
    r"\bstd::string\b"
)

AVOIDABLE_COPY_PATTERNS = (
    (
        re.compile(r"[(,]\s*const\s+std::string\s*&\s*\w*\s*[,)]"),
        "parametro 'const std::string&' que solo se lee (usa std::string_view)",
    ),
    (
        re.compile(r"\.(find|contains|count|erase|at)\(\s*std::string\("),
        "busqueda con std::string temporal (usa un mapa/set transparente y la vista)",
    ),
    (
        re.compile(r"\bstd::string\([^;]*?\)\s*\.(starts_with|ends_with|find|compare|substr|empty|size)\b"),
        "copia std::string solo para inspeccionar (opera sobre la vista)",
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


def collect_files_to_check(root: Path, steps_file: Path, extra_paths: list[str]) -> list[Path]:
    data = json.loads(steps_file.read_text(encoding="utf-8"))
    folders = set(ALWAYS_BLOCKED_UTILS)
    for f in data.get("completed_folders", []):
        if f not in FIRST_PASS_FOLDERS:
            folders.add(f)

    active = data.get("active_folder", "")
    if active and active not in FIRST_PASS_FOLDERS:
        folders.add(active)

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
    parser = argparse.ArgumentParser(description="Validador dinámico Zero-AST y Zero-std::string en 2da pasada")
    parser.add_argument("--steps", default="docs/REFACTOR_STEPS.json", help="Ruta a REFACTOR_STEPS.json")
    parser.add_argument("--extra", nargs="*", default=[], help="Archivos o carpetas adicionales a validar")
    parser.add_argument(
        "--strict-string-view",
        action="store_true",
        help="Bloquea copias evitables de std::string en 2da pasada (parametros const&, busquedas con temporal, copias de inspeccion)",
    )
    parser.add_argument(
        "--audit-string-ownership",
        action="store_true",
        help="Informe del uso total de std::string (almacenamiento propietario incluido); no hace fallar el gate",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    steps_path = root / args.steps
    if not steps_path.exists():
        sys.exit(f"[ERROR] No se encontró {steps_path}")

    files = collect_files_to_check(root, steps_path, args.extra)
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
                    f"[VIOLACIÓN AST/TS_*] {rel}:{lineno} -> '{m_ts.group(0)}' detectado en módulo de 2da pasada/util:\n    {clean}"
                )

            if (args.strict_string_view or args.audit_string_ownership) and not rel.startswith(STRING_OWNER_FOLDERS):
                if args.audit_string_ownership and BANNED_STD_STRING_REGEX.search(clean):
                    ownership_hits += 1
                reason = find_avoidable_copy(clean) if args.strict_string_view else None
                if reason:
                    violations.append(f"[VIOLACIÓN STD::STRING] {rel}:{lineno} -> {reason}:\n    {clean}")

    if args.audit_string_ownership:
        print(f"[AUDIT] {ownership_hits} lineas usan std::string (incluye almacenamiento propietario permitido).")

    if violations:
        print("\n".join(violations))
        sys.exit(f"\n[FAIL] Se encontraron {len(violations)} violaciones en carpetas verificadas/activas.")

    print(f"[PASS] {len(files)} archivos en carpetas verificadas/activas cumplen 0 ts_* y reglas de 2da pasada.")


if __name__ == "__main__":
    main()
