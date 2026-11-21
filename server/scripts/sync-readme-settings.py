#!/usr/bin/env python3
"""Render localized settings tables from the extension manifest and NLS dictionaries.

Use --update to rewrite the CONFIG_<CATEGORY> blocks in both configuration
guides, or --check to verify them. Without either flag, print the tables.
"""

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
CLIENT_DIR = REPO_ROOT / "client"

MANIFEST_PATH = CLIENT_DIR / "package.json"
NLS_EN_PATH = CLIENT_DIR / "package.nls.json"
NLS_ES_PATH = CLIENT_DIR / "package.nls.es.json"

GUIDES = {
    "en": REPO_ROOT / "docs" / "CONFIGURATION.md",
    "es": REPO_ROOT / "docs" / "CONFIGURATION.es.md",
}
CATEGORY_IDS = {
    "general": "GENERAL",
    "workspace": "WORKSPACE",
    "inlayHints": "INLAY_HINTS",
    "formatting": "FORMATTING",
    "features": "FEATURES",
    "codeLens": "CODE_LENS",
    "diagnostics": "DIAGNOSTICS",
    "engine": "ENGINE",
    "debug": "DEBUG",
}
MARKER = re.compile(r"^<!-- CONFIG_([A-Z_]+)_(START|END) -->$", re.MULTILINE)


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def format_default_value(val: object) -> str:
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
    def replace_placeholder(m: re.Match[str]) -> str:
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


def get_categories(manifest: dict) -> dict[str, dict]:
    categories = {}
    seen_settings = set()
    for category in manifest.get("contributes", {}).get("configuration", []):
        title = category.get("title", "")
        key = get_category_key(title)
        properties = category.get("properties", {})
        if key not in CATEGORY_IDS:
            settings = ", ".join(properties)
            raise ValueError(f"Category {title!r} has no configuration block: {settings}")
        if key in categories:
            raise ValueError(f"Duplicate configuration category: {key}")
        duplicates = seen_settings.intersection(properties)
        if duplicates:
            settings = ", ".join(sorted(duplicates))
            raise ValueError(f"Settings appear in more than one category: {settings}")
        seen_settings.update(properties)
        categories[key] = category

    missing = CATEGORY_IDS.keys() - categories.keys()
    if missing:
        raise ValueError(f"Missing manifest categories: {', '.join(sorted(missing))}")
    return categories


def find_blocks(path: Path, content: str) -> dict[str, tuple[int, int]]:
    blocks = {}
    markers = list(MARKER.finditer(content))
    for index in range(0, len(markers), 2):
        start = markers[index]
        category = start[1]
        if start[2] != "START" or category not in CATEGORY_IDS.values() or category in blocks:
            raise ValueError(f"{path}: unexpected marker {start[0]}")
        if index + 1 == len(markers) or markers[index + 1].groups() != (category, "END"):
            raise ValueError(f"{path}: expected CONFIG_{category}_END after CONFIG_{category}_START")
        end = markers[index + 1]
        blocks[category] = (start.end(), end.start())

    missing = set(CATEGORY_IDS.values()) - blocks.keys()
    if missing:
        raise ValueError(f"{path}: missing blocks for {', '.join(sorted(missing))}")
    return blocks


def sync_guides(catalogs: dict[str, dict[str, str]], update: bool) -> int:
    pending = {}
    for locale, path in GUIDES.items():
        relative_path = path.relative_to(REPO_ROOT)
        content = path.read_text(encoding="utf-8")
        blocks = find_blocks(relative_path, content)
        revised = content
        stale = []
        for category, (start, end) in reversed(list(blocks.items())):
            table = f"\n{catalogs[locale][category]}\n"
            if content[start:end] != table:
                stale.append(category)
                revised = revised[:start] + table + revised[end:]
        if stale:
            pending[path] = revised
            if not update:
                print(f"[FAILED] {relative_path}: outdated blocks for {', '.join(sorted(stale))}.")

    if pending and not update:
        print("Run 'python server/scripts/sync-readme-settings.py --update' to refresh the blocks.")
        return 1
    for path, content in pending.items():
        with path.open("w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"Updated {path.relative_to(REPO_ROOT)}.")
    if not pending:
        print("[PASSED] Configuration guide tables match the manifest and NLS dictionaries.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Sync and validate configuration guide tables.")
    parser.add_argument("--locale", choices=["en", "es"], default="en", help="Language for printed tables")
    parser.add_argument("--category", default="", help="Print one category (e.g. inlayHints or workspace)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--update", action="store_true", help="Update both configuration guides in place")
    mode.add_argument("--check", action="store_true", help="Verify both configuration guides")
    args = parser.parse_args()

    try:
        categories = get_categories(load_json(MANIFEST_PATH))
        dictionaries = {"en": load_json(NLS_EN_PATH), "es": load_json(NLS_ES_PATH)}
        catalogs = {
            locale: {
                CATEGORY_IDS[key]: generate_category_table(category, nls, locale)
                for key, category in categories.items()
            }
            for locale, nls in dictionaries.items()
        }
        if args.update or args.check:
            return sync_guides(catalogs, args.update)

        tables = catalogs[args.locale]
        if args.category:
            target = args.category.lower()
            for key, category in CATEGORY_IDS.items():
                if target in key.lower():
                    print(tables[category])
                    return 0
            raise ValueError(f"Category {args.category!r} not found.")
        print("\n\n".join(tables.values()))
        return 0
    except (OSError, ValueError) as error:
        print(f"[FAILED] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
