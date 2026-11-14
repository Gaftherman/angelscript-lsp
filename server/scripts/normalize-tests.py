#!/usr/bin/env python3
"""Normalizes doctest naming under server/tests.

Rules (idempotent, never drops or merges a TEST_CASE):
  * a file with exactly one TEST_SUITE gets the name "<folder>/<Stem>" (Stem = file name without "Test");
  * a file with no TEST_SUITE is wrapped in one with that name;
  * a file with several suites keeps them (they are deliberate sub-topics);
  * a TEST_CASE title without " - " becomes "<Stem> - <title>"; titles that already have it are kept.
Files with preprocessor guards (#if/#ifdef/#ifndef/#endif), TEST_SUITE_BEGIN, or top-level
namespace/using after the cut point are reported and left alone. `--dry-run` only reports.
"""
import argparse
import re
import sys
from pathlib import Path

DEFAULT_TESTS = Path(__file__).resolve().parent.parent / "tests"
SUITE = re.compile(r'TEST_SUITE\(\s*"([^"]*)"\s*\)')
CASE = re.compile(r'(TEST_CASE(?:_TEMPLATE)?\(\s*)"([^"]*)"')
GUARD = re.compile(r"^[ \t]*#(?:if|ifdef|ifndef|endif)\b", re.M)
SUITE_BEGIN = re.compile(r"\bTEST_SUITE_BEGIN\b")

_KEYWORDS = frozenset(("namespace", "using", "TEST_CASE", "TEST_CASE_TEMPLATE"))


def stem_of(path: Path) -> str:
    name = path.stem
    return name[:-4] if name.endswith("Test") and len(name) > 4 else name


def suite_name(path: Path, tests_dir: Path) -> str:
    folder = path.parent.relative_to(tests_dir).as_posix()
    return f"{folder}/{stem_of(path)}" if folder != "." else stem_of(path)


def _match_raw_string_end(text: str, pos: int) -> int | None:
    """If `pos` starts a C++ raw string literal (at 'R'), return index after closing quote."""
    n = len(text)
    if pos + 1 >= n or text[pos + 1] != '"':
        return None
    prev_ok = pos == 0 or not (text[pos - 1].isalnum() or text[pos - 1] == "_")
    if not prev_ok:
        for pref in ("u8", "u", "U", "L"):
            plen = len(pref)
            if pos >= plen and text[pos - plen : pos] == pref:
                before = pos - plen - 1
                if before < 0 or not (text[before].isalnum() or text[before] == "_"):
                    prev_ok = True
                    break
    if not prev_ok:
        return None
    paren = text.find("(", pos + 2)
    if paren == -1 or paren - (pos + 2) > 16:
        return None
    delim = text[pos + 2 : paren]
    if any(ch in delim for ch in " \t\r\n\\)"):
        return None
    end_seq = ")" + delim + '"'
    end_pos = text.find(end_seq, paren + 1)
    return n if end_pos == -1 else end_pos + len(end_seq)


def scan_cpp(text: str) -> list[tuple[str, int, int, int, str]]:
    """Scan C++ text ignoring comments, strings, raw strings, and char literals.

    Returns list of (kind, start, end, brace_depth, value).
    """
    n = len(text)
    i = 0
    depth = 0
    tokens: list[tuple[str, int, int, int, str]] = []
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j == -1 else j + 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j == -1 else j + 2
            continue
        if c == "R":
            raw_end = _match_raw_string_end(text, i)
            if raw_end is not None:
                i = raw_end
                continue
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                elif text[j] == '"':
                    j += 1
                    break
                else:
                    j += 1
            i = j
            continue
        if c == "'":
            if i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_"):
                i += 1
                continue
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                elif text[j] == "'":
                    j += 1
                    break
                else:
                    j += 1
            i = j
            continue
        if c == "{":
            tokens.append(("LBRACE", i, i + 1, depth, "{"))
            depth += 1
            i += 1
            continue
        if c == "}":
            depth = max(0, depth - 1)
            tokens.append(("RBRACE", i, i + 1, depth, "}"))
            i += 1
            continue
        if c == ";":
            tokens.append(("SEMI", i, i + 1, depth, ";"))
            i += 1
            continue
        if c == "#":
            line_start = text.rfind("\n", 0, i) + 1
            if not text[line_start:i].strip():
                j = i
                while j < n:
                    nl = text.find("\n", j)
                    if nl == -1:
                        j = n
                        break
                    if nl > 0 and text[nl - 1] == "\\":
                        j = nl + 1
                    else:
                        j = nl + 1
                        break
                tokens.append(("PP", i, j, depth, text[i:j].lstrip()))
                i = j
                continue
        if c.isalpha() or c == "_":
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            if word in _KEYWORDS:
                tokens.append(("KW", i, j, depth, word))
            i = j
            continue
        i += 1
    return tokens


def _advance_line_tail(text: str, pos: int) -> int:
    """Advance `pos` past trailing horizontal whitespace and optional // comment on the same line."""
    n = len(text)
    i = pos
    while i < n and text[i] in " \t":
        i += 1
    if i + 1 < n and text[i : i + 2] == "//":
        nl = text.find("\n", i)
        return n if nl == -1 else nl
    return pos


def _find_suite_cut(text: str) -> tuple[int | None, str | None]:
    """Find cut offset after top-level #include, using, and namespace blocks before first TEST_CASE."""
    tokens = scan_cpp(text)
    first_tc = next(
        (s for k, s, _e, _d, v in tokens if k == "KW" and v in ("TEST_CASE", "TEST_CASE_TEMPLATE")),
        None,
    )
    if first_tc is None:
        return None, "no TEST_CASE found"

    cut = 0
    idx = 0
    ntok = len(tokens)
    while idx < ntok:
        k, s, e, d, v = tokens[idx]
        if s >= first_tc:
            break
        if d == 0:
            if k == "PP" and v.startswith("#include"):
                cut = e
            elif k == "KW" and v == "using":
                j = idx + 1
                while j < ntok and not (tokens[j][0] == "SEMI" and tokens[j][3] == 0):
                    j += 1
                if j < ntok and tokens[j][2] <= first_tc:
                    cut = tokens[j][2]
                    idx = j
            elif k == "KW" and v == "namespace":
                j = idx + 1
                while j < ntok and not (tokens[j][3] == 0 and tokens[j][0] in ("LBRACE", "SEMI")):
                    j += 1
                if j < ntok and tokens[j][0] == "SEMI":
                    if tokens[j][2] <= first_tc:
                        cut = tokens[j][2]
                    idx = j
                elif j < ntok and tokens[j][0] == "LBRACE":
                    m = j + 1
                    while m < ntok and not (tokens[m][0] == "RBRACE" and tokens[m][3] == 0):
                        m += 1
                    if m < ntok:
                        if tokens[m][2] <= first_tc:
                            cut = _advance_line_tail(text, tokens[m][2])
                        idx = m
        idx += 1

    remaining = [
        v for k, s, _e, d, v in tokens if s >= cut and d == 0 and k == "KW" and v in ("namespace", "using")
    ]
    if remaining:
        return None, "top-level namespace/using after cut"
    return cut, None


def normalize(path: Path, text: str, tests_dir: Path) -> tuple[str, str | None]:
    """Return (new_text, skipped_reason or None)."""
    if "TEST_CASE" not in text:
        return text, None
    if GUARD.search(text):
        return text, "preprocessor guard"
    if SUITE_BEGIN.search(text):
        return text, "TEST_SUITE_BEGIN"

    suites = SUITE.findall(text)
    stem = stem_of(path)
    name = suite_name(path, tests_dir)

    if not suites:
        cut, skip_reason = _find_suite_cut(text)
        if skip_reason:
            return text, skip_reason
        assert cut is not None
        text = CASE.sub(
            lambda m: m.group(0) if " - " in m.group(2) else f'{m.group(1)}"{stem} - {m.group(2)}"',
            text,
        )
        prefix = text[:cut].rstrip("\n")
        body = text[cut:].strip("\n")
        if prefix:
            return f'{prefix}\n\nTEST_SUITE("{name}")\n{{\n{body}\n}}\n', None
        return f'TEST_SUITE("{name}")\n{{\n{body}\n}}\n', None

    text = CASE.sub(
        lambda m: m.group(0) if " - " in m.group(2) else f'{m.group(1)}"{stem} - {m.group(2)}"',
        text,
    )
    if len(suites) == 1:
        return SUITE.sub(f'TEST_SUITE("{name}")', text, count=1), None
    return text, None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tests", type=Path, default=DEFAULT_TESTS)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    tests_dir = args.tests.resolve()
    changed, skipped = 0, []
    for path in sorted(tests_dir.rglob("*.cpp")):
        raw = path.read_bytes()
        bom = raw.startswith(b"\xef\xbb\xbf")
        original = raw.decode("utf-8-sig")
        crlf = "\r\n" in original
        updated, reason = normalize(path, original.replace("\r\n", "\n"), tests_dir)
        if reason:
            skipped.append(f"{path.relative_to(tests_dir).as_posix()} ({reason})")
            continue
        if crlf:
            updated = updated.replace("\n", "\r\n")
        if updated != original:
            changed += 1
            if not args.dry_run:
                out_bytes = updated.encode("utf-8")
                if bom:
                    out_bytes = b"\xef\xbb\xbf" + out_bytes
                path.write_bytes(out_bytes)
    print(f"files {'to change' if args.dry_run else 'changed'}: {changed}; skipped: {len(skipped)}")
    for item in skipped:
        print("  skipped", item)
    return 0


if __name__ == "__main__":
    sys.exit(main())
