#!/usr/bin/env python3
"""
Synchronizes CHANGELOG.md release notes into all GitHub Releases for Gaftherman/angelscript-lsp.

Usage:
    python server/scripts/sync-github-releases.py --dry-run
    python server/scripts/sync-github-releases.py
    python server/scripts/sync-github-releases.py --tag v1.1.6-exp.1
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
REPO_NAME = "Gaftherman/angelscript-lsp"


def get_github_token() -> str:
    # 1. Environment variables
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        return token

    # 2. Git Credential Manager
    try:
        proc = subprocess.Popen(
            ["git", "credential", "fill"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        input_data = f"protocol=https\nhost=github.com\npath={REPO_NAME}.git\nusername=Gaftherman\n"
        out, _ = proc.communicate(input_data)
        for line in out.splitlines():
            if line.startswith("password="):
                return line.split("=", 1)[1].strip()
    except Exception as e:
        print(f"Warning: Could not retrieve token from git credential: {e}", file=sys.stderr)

    return ""


def load_changelog(changelog_path: Path) -> dict[str, str]:
    with open(changelog_path, "r", encoding="utf-8") as f:
        text = f.read()

    pattern = r"^##\s+\[(.*?)\](?:\s*-\s*([^\n\r]+))?"
    matches = list(re.finditer(pattern, text, re.MULTILINE))
    notes_by_ver: dict[str, str] = {}

    for i, m in enumerate(matches):
        ver = m.group(1).strip().lstrip("v")
        start = m.end()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[start:end].strip()
        notes_by_ver[ver] = body

    return notes_by_ver


def api_request(url: str, token: str, data: dict | None = None, method: str = "GET") -> dict | list:
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "User-Agent": "AngelLSP-Release-Sync",
    }
    encoded_data = json.dumps(data).encode("utf-8") if data is not None else None
    req = urllib.request.Request(url, data=encoded_data, headers=headers, method=method)
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fetch_all_releases(token: str) -> list[dict]:
    releases = []
    page = 1
    while True:
        url = f"https://api.github.com/repos/{REPO_NAME}/releases?per_page=100&page={page}"
        batch = api_request(url, token)
        if not batch:
            break
        releases.extend(batch)
        if len(batch) < 100:
            break
        page += 1
    return releases


def deduce_prev_tag(tag_name: str, all_releases: list[dict]) -> str | None:
    # 1. Experimental tag (e.g., v1.1.6-exp.8 -> v1.1.6-exp.7; v1.1.6-exp.1 -> v1.1.5)
    m = re.match(r"^v?(\d+\.\d+\.\d+)-exp\.(\d+)$", tag_name)
    if m:
        base_ver = m.group(1)
        exp_num = int(m.group(2))
        if exp_num > 1:
            return f"v{base_ver}-exp.{exp_num - 1}"
        parts = [int(p) for p in base_ver.split(".")]
        if len(parts) == 3 and parts[2] > 0:
            return f"v{parts[0]}.{parts[1]}.{parts[2] - 1}"
        elif len(parts) == 3 and parts[1] > 0:
            return f"v{parts[0]}.{parts[1] - 1}.0"

    # 2. Stable release (e.g., v1.1.6 -> v1.1.5)
    m_stable = re.match(r"^v?(\d+\.\d+\.\d+)$", tag_name)
    if m_stable:
        base_ver = m_stable.group(1)
        parts = [int(p) for p in base_ver.split(".")]
        if len(parts) == 3 and parts[2] > 0:
            return f"v{parts[0]}.{parts[1]}.{parts[2] - 1}"
        elif len(parts) == 3 and parts[1] > 0:
            return f"v{parts[0]}.{parts[1] - 1}.0"

    # 3. Fallback: find chronological predecessor in all_releases
    for i, r in enumerate(all_releases):
        if r.get("tag_name") == tag_name:
            if i + 1 < len(all_releases):
                return all_releases[i + 1].get("tag_name")
            break

    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Sync CHANGELOG.md notes into GitHub Releases.")
    parser.add_argument("--dry-run", action="store_true", help="Print updates without modifying GitHub Releases")
    parser.add_argument("--tag", type=str, default="", help="Only sync a specific release tag")
    parser.add_argument("--force", action="store_true", help="Force update even if body appears unchanged")
    parser.add_argument("--changelog", type=Path, default=REPO_ROOT / "CHANGELOG.md", help="Path to CHANGELOG.md")

    args = parser.parse_args()

    token = get_github_token()
    if not token:
        print("Error: Could not obtain GitHub authentication token.", file=sys.stderr)
        return 1

    notes_by_ver = load_changelog(args.changelog)
    print(f"Loaded {len(notes_by_ver)} versions from {args.changelog}")

    print("Fetching releases from GitHub...")
    all_releases = fetch_all_releases(token)
    print(f"Found {len(all_releases)} releases on GitHub.")

    releases = all_releases
    if args.tag:
        releases = [r for r in all_releases if r.get("tag_name") == args.tag or r.get("tag_name") == f"v{args.tag.lstrip('v')}"]
        if not releases:
            print(f"Error: Release with tag '{args.tag}' not found on GitHub.", file=sys.stderr)
            return 1

    updated_count = 0
    unchanged_count = 0
    missing_count = 0

    for rel in releases:
        tag_name = rel.get("tag_name", "")
        clean_ver = tag_name.lstrip("v")
        rel_id = rel.get("id")
        current_body = rel.get("body") or ""

        if clean_ver not in notes_by_ver:
            print(f"[MISSING] No changelog entry for release {tag_name}")
            missing_count += 1
            continue

        changelog_content = notes_by_ver[clean_ver]

        # Preserve existing compare link if present, or deduce from predecessor tag
        compare_match = re.search(r"(\*\*Full Changelog\*\*:\s*https://github\.com/\S+)", current_body)
        if compare_match:
            full_body = f"{changelog_content}\n\n---\n{compare_match.group(1)}"
        else:
            prev_tag = deduce_prev_tag(tag_name, all_releases)
            if prev_tag:
                compare_link = f"**Full Changelog**: https://github.com/{REPO_NAME}/compare/{prev_tag}...{tag_name}"
                full_body = f"{changelog_content}\n\n---\n{compare_link}"
            else:
                full_body = changelog_content

        if full_body.strip() == current_body.strip() and not args.force:
            print(f"[UNCHANGED] {tag_name} already matches changelog.")
            unchanged_count += 1
            continue

        if args.dry_run:
            print(f"[DRY-RUN] Would update {tag_name} (ID: {rel_id}): {len(full_body)} chars")
            updated_count += 1
        else:
            try:
                patch_url = f"https://api.github.com/repos/{REPO_NAME}/releases/{rel_id}"
                api_request(patch_url, token, data={"body": full_body}, method="PATCH")
                print(f"[UPDATED] {tag_name} successfully updated ({len(full_body)} chars).")
                updated_count += 1
                time.sleep(0.2)  # Avoid rate limit spikes
            except urllib.error.HTTPError as err:
                print(f"[ERROR] Failed to update {tag_name}: {err}", file=sys.stderr)

    print("\n=======================================================")
    print(f"Summary: {updated_count} updated, {unchanged_count} unchanged, {missing_count} missing.")
    print("=======================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
