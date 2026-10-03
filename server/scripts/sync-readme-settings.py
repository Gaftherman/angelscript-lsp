#!/usr/bin/env python3
"""Synchronize and validate settings catalog tables in README files from client/package.json and NLS dictionaries.

This script acts as the Single Source of Truth generator for settings documentation:
  1. Reads all declared settings, types, and defaults directly from client/package.json.
  2. Resolves localized descriptions from client/package.nls.json (English) and client/package.nls.es.json (Spanish).
  3. Outputs formatted Markdown tables for any category or the entire catalog.
  4. With --update, automatically syncs the tables into client/README.md and client/README.es.md between:
     <!-- SETTINGS_CATALOG_START --> ... <!-- SETTINGS_CATALOG_END -->
  5. With --check, asserts that the documentation matches package.json and NLS tables (for CI / check-quality.py).
"""

import argparse
import io
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
CLIENT_DIR = REPO_ROOT / "client"

MANIFEST_PATH = CLIENT_DIR / "package.json"
NLS_EN_PATH = CLIENT_DIR / "package.nls.json"
NLS_ES_PATH = CLIENT_DIR / "package.nls.es.json"

CLIENT_README_EN = CLIENT_DIR / "README.md"
CLIENT_README_ES = CLIENT_DIR / "README.es.md"
ROOT_README_EN = REPO_ROOT / "README.md"
ROOT_README_ES = REPO_ROOT / "README.es.md"

CATALOG_START_MARKER = "<!-- SETTINGS_CATALOG_START -->"
CATALOG_END_MARKER = "<!-- SETTINGS_CATALOG_END -->"


def load_json(path: Path):
    with io.open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def format_default_value(val) -> str:
    if val is None:
        return "null"
    if isinstance(val, bool):
        return "true" if val else "false"
    if isinstance(val, (int, float)):
        return str(val)
    if isinstance(val, str):
        return f'"{val}"'
    if isinstance(val, list):
        if not val:
            return "[]"
        items = ", ".join(f'"{x}"' if isinstance(x, str) else str(x) for x in val)
        return f"[{items}]"
    if isinstance(val, dict):
        if not val:
            return "{}"
        return json.dumps(val)
    return str(val)


def resolve_string(raw: str, nls_dict: dict) -> str:
    if not isinstance(raw, str):
        return ""
    # Replace any %key% placeholders with translation
    def replace_placeholder(m):
        k = m.group(1)
        return nls_dict.get(k, m.group(0))

    resolved = re.sub(r"%([^%]+)%", replace_placeholder, raw)
    # Sanitize markdown table characters: replace pipe and normalize whitespace
    resolved = resolved.replace("|", "\\|").replace("\r\n", " ").replace("\n", " ").strip()
    return resolved


def get_category_key(title: str) -> str:
    # e.g. "%config.category.inlayHints%" -> "inlayHints"
    m = re.search(r"category\.([a-zA-Z0-9]+)", title)
    if m:
        return m.group(1)
    return title.strip("%")


def generate_category_table(category_config: dict, nls_dict: dict, locale: str) -> str:
    headers = {
        "en": ("Setting", "Default", "Description"),
        "es": ("Configuración", "Valor por defecto", "Descripción"),
    }
    col_setting, col_default, col_desc = headers.get(locale, headers["en"])

    lines = [
        f"| {col_setting} | {col_default} | {col_desc} |",
        "| :--- | :--- | :--- |",
    ]

    properties = category_config.get("properties", {})
    for prop_name, prop_spec in properties.items():
        raw_desc = prop_spec.get("markdownDescription") or prop_spec.get("description", "")
        desc = resolve_string(raw_desc, nls_dict)
        default_val = format_default_value(prop_spec.get("default"))
        lines.append(f"| `{prop_name}` | `{default_val}` | {desc} |")

    return "\n".join(lines)


def generate_full_catalog(manifest: dict, nls_dict: dict, locale: str) -> str:
    category_titles = {
        "general": {"en": "1. General & Server Configuration", "es": "1. Configuración general y del servidor"},
        "workspace": {"en": "2. Workspace & Script Modules Configuration", "es": "2. Configuración del espacio de trabajo y módulos"},
        "inlayHints": {"en": "3. Inlay Hints Configuration", "es": "3. Configuración de Inlay Hints"},
        "formatting": {"en": "4. Code Formatting Configuration", "es": "4. Configuración de formato de código"},
        "features": {"en": "5. Language Features & Autocompletion Configuration", "es": "5. Configuración de características LSP y autocompletado"},
        "diagnostics": {"en": "6. Semantic Diagnostics & Analysis Configuration", "es": "6. Configuración de diagnósticos semánticos y análisis"},
        "engine": {"en": "7. Engine Dialect & Preprocessor Configuration (asEP_*)", "es": "7. Configuración del dialecto del motor y preprocesador (asEP_*)"},
    }

    configurations = manifest.get("contributes", {}).get("configuration", [])
    sections = []

    for idx, cat_spec in enumerate(configurations, start=1):
        raw_title = cat_spec.get("title", "")
        cat_key = get_category_key(raw_title)

        title_lookup = category_titles.get(cat_key, {})
        section_heading = title_lookup.get(locale, resolve_string(raw_title, nls_dict))
        table = generate_category_table(cat_spec, nls_dict, locale)

        sections.append(f"#### {section_heading}\n\n{table}")

    return "\n\n".join(sections)


def inject_catalog_into_readme(file_path: Path, catalog_markdown: str) -> bool:
    if not file_path.exists():
        return False

    with io.open(file_path, "r", encoding="utf-8") as f:
        content = f.read()

    pattern = re.compile(
        rf"({re.escape(CATALOG_START_MARKER)}\n)(.*?)(\n{re.escape(CATALOG_END_MARKER)})",
        re.DOTALL,
    )

    if pattern.search(content):
        new_content = pattern.sub(rf"\g<1>{catalog_markdown}\g<3>", content)
    else:
        # If markers are not yet present, look for '### Settings Catalog' or '### Catálogo de configuraciones'
        header_patterns = [
            r"### Settings Catalog\n\n",
            r"### Catálogo de configuraciones\n\n",
        ]
        inserted = False
        for hp in header_patterns:
            m = re.search(hp, content)
            if m:
                # Find next top-level separator '---' or section '## '
                end_m = re.search(r"\n---\n", content[m.end():])
                if end_m:
                    catalog_slice_end = m.end() + end_m.start()
                    replacement = (
                        f"{CATALOG_START_MARKER}\n\n{catalog_markdown}\n\n{CATALOG_END_MARKER}"
                    )
                    new_content = (
                        content[: m.end()] + replacement + content[catalog_slice_end:]
                    )
                    inserted = True
                    break
        if not inserted:
            return False

    if new_content != content:
        with io.open(file_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(new_content)
        return True

    return False


def verify_readme(file_path: Path, catalog_markdown: str) -> bool:
    if not file_path.exists():
        return False
    with io.open(file_path, "r", encoding="utf-8") as f:
        content = f.read()

    pattern = re.compile(
        rf"{re.escape(CATALOG_START_MARKER)}\n(.*?)\n{re.escape(CATALOG_END_MARKER)}",
        re.DOTALL,
    )
    m = pattern.search(content)
    if not m:
        return False
    existing = m.group(1).strip()
    return existing == catalog_markdown.strip()


def main():
    parser = argparse.ArgumentParser(
        description="Sync and validate settings catalog tables from package.json."
    )
    parser.add_argument("--locale", choices=["en", "es"], default="en", help="Language for output (en or es)")
    parser.add_argument("--category", type=str, default="", help="Specific category to print (e.g. inlayHints, workspace, engine, or empty for all)")
    parser.add_argument("--update", action="store_true", help="Update client/README.md and client/README.es.md in-place")
    parser.add_argument("--check", action="store_true", help="Check that client/README.md and client/README.es.md match package.json")

    args = parser.parse_args()

    manifest = load_json(MANIFEST_PATH)
    nls_en = load_json(NLS_EN_PATH)
    nls_es = load_json(NLS_ES_PATH)

    catalog_en = generate_full_catalog(manifest, nls_en, "en")
    catalog_es = generate_full_catalog(manifest, nls_es, "es")

    if args.update:
        updated_en = inject_catalog_into_readme(CLIENT_README_EN, catalog_en)
        updated_es = inject_catalog_into_readme(CLIENT_README_ES, catalog_es)
        print(f"Updated client/README.md (EN): {updated_en}")
        print(f"Updated client/README.es.md (ES): {updated_es}")
        return 0

    if args.check:
        ok_en = verify_readme(CLIENT_README_EN, catalog_en)
        ok_es = verify_readme(CLIENT_README_ES, catalog_es)
        if not ok_en or not ok_es:
            print("[FAILED] README settings catalogs are out of date with client/package.json.")
            if not ok_en:
                print("  - client/README.md does not match generated English catalog or markers missing.")
            if not ok_es:
                print("  - client/README.es.md does not match generated Spanish catalog or markers missing.")
            print("Run 'python server/scripts/sync-readme-settings.py --update' to re-synchronize.")
            return 1
        print("[PASSED] README settings catalogs are completely synchronized with package.json.")
        return 0

    # Standalone print mode
    nls = nls_es if args.locale == "es" else nls_en
    if args.category:
        target = args.category.lower()
        matched = False
        for cat in manifest.get("contributes", {}).get("configuration", []):
            cat_key = get_category_key(cat.get("title", "")).lower()
            if target in cat_key:
                table = generate_category_table(cat, nls, args.locale)
                print(table)
                matched = True
                break
        if not matched:
            print(f"Error: Category '{args.category}' not found.", file=sys.stderr)
            return 1
    else:
        print(catalog_es if args.locale == "es" else catalog_en)

    return 0


if __name__ == "__main__":
    sys.exit(main())
