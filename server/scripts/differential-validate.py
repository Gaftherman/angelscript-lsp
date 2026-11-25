#!/usr/bin/env python3
"""Differential validation of the language server against the AngelScript oracle.

Every case lives in server/tests/parity/differential/<category>/ as either `<id>.as` or a directory
`<id>/main.as` plus sibling files (reached through #include). Header comments in the main file:

    // @expect: accept|reject          what the real compiler must say about the script
    // @complete+ Label                 Label must compile when it replaces the marked prefix
    // @complete- Label                 Label must NOT compile there
    // @scan: all                       every non-keyword completion the server returns must compile
    // @error: L:C-L:C                  exact 0-based diagnostic range in main.as
    // @error: file.as code L:C-L:C     exact diagnostic file, LSP code, and 0-based range

A file may hold many cases, each introduced by `// ==== case: <id> ====`; every section is compiled
and analysed on its own, so names never collide between sections.

`/*$complete*/` marks the cursor and sits right after the partial identifier, so replacing that
identifier by a candidate yields a script the oracle can judge. The oracle runs first: a case whose
header disagrees with it is reported as ORACLE_CONTRADICTS_CASE and never reaches the server. Only
confirmed cases are replayed against `angel_lsp --mcp`, and each disagreement is written to the log.
Diagnostic identity is the case-relative file, code and full zero-based UTF-16 range.
After exact identities pair, remaining identities match by case-relative file and full range.
Compiler diagnostics without identity match by case-relative file and UTF-16 start position.
Unpaired positions are informational when both tools report errors in the same file.
Server errors in extra files are informational when the oracle reports an error in the case.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from queue import Empty, Queue
from threading import Thread

SERVER_DIR = Path(__file__).resolve().parent.parent
MARKER = "/*$complete*/"
KIND_KEYWORD, KIND_SNIPPET = 14, 15


DUMP = None  # Set by --dump-ranges: case id -> file -> server error ranges.
CASE_MARKER = re.compile(r"^//\s*====\s*case:\s*(\S+)\s*====\s*$")
ERROR_TAG = re.compile(r"\s*//\s*@error:\s*(?:(\S+)\s+(\S+)\s+)?(\d+):(\d+)-(\d+):(\d+)\s*$")


@dataclass(frozen=True)
class OracleError:
    file: str | None
    code: str | int | None
    span: tuple[int, int, int, int] | None
    row: int
    column: int
    message: str


class OracleFailure(RuntimeError):
    """The compiler process could not produce a usable verdict."""


SYNTHETIC_SECTIONS = {"default arg"}  # The SDK reparses a default expression in its own temporary section.


def split_cases(text: str):
    """Splits a multi-case file into [(id, body)]; a file without markers is one case with id ''."""
    sections, current_id, body = [], "", []
    for line in text.splitlines(keepends=True):
        m = CASE_MARKER.match(line.strip())
        if m:
            if current_id or any(part.strip() for part in body):
                sections.append((current_id, "".join(body)))
            current_id, body = m.group(1), []
        else:
            body.append(line)
    if current_id or any(part.strip() for part in body):
        sections.append((current_id, "".join(body)))
    return sections


class Case:
    """One corpus case. Multi-case files contribute one Case per `// ==== case: id ====` section.

    An @error tag pins a diagnostic by file, optional code and exact range relative to its source.
    """

    def __init__(self, path: Path, case_id: str = "", text: str = None):
        self.path = path
        self.main = path / "main.as" if path.is_dir() else path
        self.id = path.name if path.is_dir() else (f"{path.stem}::{case_id}" if case_id else path.stem)
        self.category = path.parent.name
        self.text = self.main.read_text(encoding="utf-8") if text is None else text
        self.expect = None
        self.plus, self.minus, self.scan_all, self.ranges = [], [], False, []
        # `// @predefined: <fixture>` serves tests/fixtures/<fixture>.as.predefined to the server as the
        # workspace stub; the oracle registers the same add-ons natively, so it needs nothing.
        self.stub = None
        for line in self.text.splitlines():
            m = re.match(r"\s*//\s*@(expect|scan):\s*(\w+)", line)
            if m:
                if m.group(1) == "expect":
                    self.expect = m.group(2)
                else:
                    self.scan_all = m.group(2) == "all"
            m = re.match(r"\s*//\s*@predefined:\s*([\w.-]+)", line)
            if m:
                self.stub = m.group(1)
            m = re.match(r"\s*//\s*@complete([+-])\s*(\S+)", line)
            if m:
                (self.plus if m.group(1) == "+" else self.minus).append(m.group(2))
            m = ERROR_TAG.match(line)
            if m:
                file, code, *coordinates = m.groups()
                self.ranges.append((file or "main.as", code, tuple(map(int, coordinates))))

    @staticmethod
    def load(path: Path):
        """Returns every Case an entry holds: directories and plain files are one, multi-case files many."""
        if path.is_dir():
            return [Case(path)]
        sections = split_cases(path.read_text(encoding="utf-8"))
        if len(sections) == 1 and sections[0][0] == "":
            return [Case(path)]
        ids = [s[0] for s in sections]
        if len(set(ids)) != len(ids):
            raise ValueError(f"duplicate case ids in {path.name}")
        return [Case(path, cid, body) for cid, body in sections]

    def siblings(self):
        if not self.path.is_dir():
            return {}
        return {
            p.name: p.read_text(encoding="utf-8")
            for p in self.path.iterdir()
            if p.suffix == ".as" and p.name != "main.as"
        }

    def marker(self):
        """Returns (text without marker, prefix, line, character, offset) or None."""
        index = self.text.find(MARKER)
        if index < 0:
            return None
        before = self.text[:index]
        m = re.search(r"[A-Za-z_]\w*$|$", before)
        prefix = m.group(0)
        clean = before + self.text[index + len(MARKER) :]
        line = before.count("\n")
        character = len(before.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2
        return clean, prefix, line, character, index


def run_oracle(oracle: Path, files: dict, main: str):
    """Compile the case and return its verdict and diagnostics with case-relative file names."""
    work = Path(tempfile.mkdtemp(prefix="diff_oracle_")).resolve()
    try:
        for name, text in files.items():
            (work / name).write_text(text, encoding="utf-8")
        proc = subprocess.run(
            [str(oracle.resolve()), str(work / main), "--json", "--no-pause", "--no-mixin-probe"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=60,
            cwd=work,
        )
        if proc.returncode not in (0, 1):
            raise OracleFailure(f"compiler exit {proc.returncode}: {proc.stderr.strip()[:240]}")
        sections = {(work / name).resolve(): name for name in files}
        try:
            report = json.loads(proc.stdout)
            rejected = report["hasError"]
            if type(rejected) is not bool or not isinstance(report["messages"], list):
                raise ValueError("invalid verdict or message list")
            errors = []
            for message in report["messages"]:
                kind = message["type"]
                if kind not in ("ERROR", "WARNING", "INFO"):
                    raise ValueError("invalid compiler message type")
                if kind != "ERROR":
                    continue
                section = message["section"]
                row, column = message["row"], message["col"]
                detail = message["message"]
                if (
                    not isinstance(section, str)
                    or type(row) is not int
                    or type(column) is not int
                    or not isinstance(detail, str)
                ):
                    raise ValueError("invalid compiler diagnostic")
                file = None
                if section and section not in SYNTHETIC_SECTIONS:
                    path = (work / section).resolve()
                    file = sections.get(path)
                    if file is None:
                        file = path.relative_to(work).as_posix() if path.is_relative_to(work) else path.as_posix()
                code, span = diagnostic_identity(message)
                if (code is None) != (span is None) or (span is not None and file is None):
                    raise ValueError("incomplete compiler diagnostic identity")
                errors.append(OracleError(file, code, span, row, column, detail))
            if rejected != bool(errors) or proc.returncode != int(rejected):
                raise ValueError("compiler exit, verdict and error messages disagree")
        except (ValueError, KeyError, TypeError) as error:
            raise OracleFailure(f"invalid compiler JSON: {error}") from error
        return not rejected, errors
    except (OSError, UnicodeError, subprocess.TimeoutExpired) as error:
        raise OracleFailure(str(error)) from error
    finally:
        shutil.rmtree(work, ignore_errors=True)


class McpClient:
    def __init__(self, exe: Path, root: Path):
        self.proc = subprocess.Popen(
            [str(exe), "--mcp", "--mcp-root", str(root)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        self.replies = Queue()
        self.reader = Thread(target=self._read_replies, daemon=True)
        self.reader.start()
        self.n = 0
        try:
            self._rpc(
                "initialize",
                {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {},
                    "clientInfo": {"name": "differential", "version": "1"},
                },
            )
            self._rpc("notifications/initialized", notify=True)
        except Exception:
            self.close()
            raise

    def _read_replies(self):
        try:
            for line in self.proc.stdout:
                self.replies.put(line)
        except (OSError, UnicodeError) as error:
            self.replies.put(error)
        finally:
            self.replies.put(None)

    def _rpc(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            self.n += 1
            msg["id"] = self.n
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        if notify:
            return None
        try:
            line = self.replies.get(timeout=60)
        except Empty as error:
            raise RuntimeError(f"no MCP response for {method} within 60 seconds") from error
        if line is None:
            raise RuntimeError(f"MCP transport closed while waiting for {method}")
        if isinstance(line, Exception):
            raise RuntimeError(f"MCP transport failure while waiting for {method}: {line}") from line
        reply = json.loads(line)
        if not isinstance(reply, dict) or reply.get("id") != self.n or "result" not in reply:
            raise RuntimeError(f"unexpected MCP response for {method}: {reply}")
        return reply

    def tool(self, name, **args):
        reply = self._rpc("tools/call", {"name": name, "arguments": args})
        res = reply["result"]
        text = res["content"][0]["text"]
        if res["isError"]:
            raise RuntimeError(text)
        return json.loads(text)

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            self.proc.kill()
            self.proc.wait()
        self.reader.join()
        self.proc.stdout.close()


def diagnostic_range(diagnostic):
    start, end = diagnostic["range"]["start"], diagnostic["range"]["end"]
    return start["line"], start["character"], end["line"], end["character"]


def diagnostic_identity(diagnostic):
    code = diagnostic["code"]
    if code is not None and (type(code) not in (str, int) or code == ""):
        raise ValueError("invalid code")
    span = None
    if diagnostic["range"] is not None:
        span = diagnostic_range(diagnostic)
        if any(type(value) is not int or value < 0 for value in span) or span[:2] > span[2:]:
            raise ValueError("invalid range")
    return code, span


def read_error_diagnostics(client, file):
    """Validate the MCP diagnostic report before selecting its errors."""
    try:
        report = client.tool("diagnostics", path=file)
    except (OSError, KeyError, TypeError, ValueError, RuntimeError) as error:
        raise ValueError(f"diagnostics for {file}: {error}") from error
    if not isinstance(report, list):
        raise ValueError(f"diagnostics for {file}: expected an array")

    errors = []
    for index, diagnostic in enumerate(report):
        try:
            if not isinstance(diagnostic, dict):
                raise ValueError("expected an object")
            severity = diagnostic["severity"]
            if type(severity) is not int or not 1 <= severity <= 4:
                raise ValueError("invalid severity")
            if not isinstance(diagnostic["message"], str):
                raise ValueError("invalid message")
            _, span = diagnostic_identity({"code": diagnostic.get("code"), "range": diagnostic["range"]})
            if span is None:
                raise ValueError("invalid range")
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError(f"diagnostics for {file}, item {index + 1}: {error}") from error
        if severity == 1:
            errors.append(diagnostic)
    return errors


def oracle_start(error, source_lines):
    """Convert a compiler byte position to an LSP start, or return None if invalid."""
    lines = source_lines.get(error.file, ())
    if not 1 <= error.row <= len(lines) or error.column < 1:
        return None
    line = lines[error.row - 1].encode("utf-8")
    offset = error.column - 1
    if offset > len(line):
        return None
    try:
        prefix = line[:offset].decode("utf-8")
    except UnicodeDecodeError:
        return None
    return error.row - 1, len(prefix.encode("utf-16-le")) // 2


def compare_diagnostics(reports, oracle_errors, files):
    """Pair diagnostics and classify the remaining identities, positions and verdicts."""
    server_files = {file for file, diagnostics in reports.items() if diagnostics}
    oracle_files = {error.file for error in oracle_errors if error.file is not None}
    shared_files = server_files & oracle_files
    server = Counter(
        (file, diagnostic.get("code"), diagnostic_range(diagnostic))
        for file, diagnostics in reports.items()
        for diagnostic in diagnostics
    )
    oracle = Counter((error.file, error.code, error.span) for error in oracle_errors if error.code is not None)
    exact = server & oracle
    matched = sum(exact.values())
    server -= exact
    oracle -= exact

    if oracle:
        server_ranges = defaultdict(list)
        for identity in server.elements():
            file, _, span = identity
            server_ranges[file, span].append(identity)
        for identity in oracle.elements():
            file, _, span = identity
            candidates = server_ranges.get((file, span))
            if candidates:
                server[candidates.pop()] -= 1
                oracle[identity] -= 1
                matched += 1

    unmatched_positions = []
    unidentified = [error for error in oracle_errors if error.code is None]
    if unidentified:
        source_lines = {file: text.split("\n") for file, text in files.items()}
        server_starts = defaultdict(list)
        for identity in server.elements():
            file, _, span = identity
            server_starts[file, span[:2]].append(identity)
        for error in unidentified:
            candidates = server_starts.get((error.file, oracle_start(error, source_lines)))
            if candidates:
                server[candidates.pop()] -= 1
                matched += 1
            else:
                unmatched_positions.append(error)

    remaining_oracle = defaultdict(list)
    for identity in oracle.elements():
        remaining_oracle[identity[1]].append(identity)

    findings = []
    for identity in server.elements():
        candidates = remaining_oracle.get(identity[1])
        if candidates:
            findings.append(("RANGE_FILE_MISMATCH", f"server {identity!r}; oracle {candidates.pop()!r}", "failing"))
        elif identity[0] in shared_files:
            findings.append(("SERVER_ONLY", repr(identity), "informational"))
    findings.extend(
        ("ORACLE_ONLY", repr(identity), "failing") for group in remaining_oracle.values() for identity in group
    )
    findings.extend(
        ("ORACLE_ONLY", f"{error.file}:{error.row}:{error.column}: {error.message}", "informational")
        for error in unmatched_positions
        if error.file in shared_files
    )

    # Synthetic sections can establish rejection at case level when the oracle has no file attribution.
    if oracle_errors and not oracle_files and not server_files:
        findings.append(("VERDICT_MISMATCH", "case: oracle reports errors, server reports none", "failing"))
    for file in sorted(oracle_files - server_files):
        findings.append(("VERDICT_MISMATCH", f"{file}: oracle reports errors, server reports none", "failing"))
    for file in sorted(server_files - oracle_files):
        detail = f"{file}: server reports errors, oracle reports none"
        if oracle_errors:
            findings.append(("SERVER_EXTRA_FILE", detail, "informational"))
        else:
            findings.append(("VERDICT_MISMATCH", detail, "failing"))
    return matched, findings


def evaluate(case: Case, oracle: Path, lsp_exe: Path):
    try:
        return evaluate_case(case, oracle, lsp_exe)
    except OracleFailure as error:
        return 0, [
            {
                "case": f"{case.category}/{case.id}",
                "kind": "ORACLE_ERROR",
                "detail": str(error),
                "classification": "oracle_failure",
            }
        ]
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        return 0, [
            {
                "case": f"{case.category}/{case.id}",
                "kind": "HARNESS_ERROR",
                "detail": str(error),
                "classification": "harness_error",
            }
        ]


def evaluate_case(case: Case, oracle: Path, lsp_exe: Path):
    findings = []
    matched = 0

    def add(kind, detail, classification="failing"):
        if kind == "ORACLE_CONTRADICTS_CASE":
            classification = "oracle_contradiction"
        findings.append(
            {"case": f"{case.category}/{case.id}", "kind": kind, "detail": detail, "classification": classification}
        )

    files = {"main.as": case.text.replace(MARKER, "")}
    files.update(case.siblings())
    marker = case.marker()
    if marker:
        files["main.as"] = marker[0]

    # With a marker the text is a half-typed script, so the compiler can only judge the completed
    # forms below; diagnostics are compared only for cases without a marker.
    accepted, errors = (True, []) if marker else run_oracle(oracle, files, "main.as")
    if not marker and case.expect and accepted != (case.expect == "accept"):
        add(
            "ORACLE_CONTRADICTS_CASE",
            f"header says {case.expect}, oracle {'accepts' if accepted else 'rejects'}: {errors[:1]}",
        )
        return matched, findings

    # Candidate verdicts from the oracle, decided before the server is ever asked.
    verdict = {}
    if marker:
        clean, prefix, _, _, _ = marker
        stem = clean[: marker[4] - len(prefix)]
        tail = clean[marker[4] :]

        def judge(label):
            if label not in verdict:
                body = dict(files)
                body["main.as"] = stem + label + tail
                verdict[label] = run_oracle(oracle, body, "main.as")[0]
            return verdict[label]

        for label in case.plus:
            if not judge(label):
                add("ORACLE_CONTRADICTS_CASE", f"@complete+ {label} does not compile")
        for label in case.minus:
            if judge(label):
                add("ORACLE_CONTRADICTS_CASE", f"@complete- {label} compiles")
        if findings:
            return matched, findings

    root = Path(tempfile.mkdtemp(prefix="diff_lsp_"))
    client = None
    try:
        for name, text in files.items():
            (root / name).write_text(text, encoding="utf-8")
        if case.stub:
            fixtures = SERVER_DIR / "tests" / "fixtures"
            shutil.copyfile(fixtures / f"{case.stub}.as.predefined", root / "as.predefined")
            rules = fixtures / f"{case.stub}.angelscript.rules.json"
            if rules.exists():
                shutil.copyfile(rules, root / rules.name)
        client = McpClient(lsp_exe, root)
        for name, text in files.items():
            client.tool("open_document", path=name, text=text)
        if not marker:
            diags = {name: read_error_diagnostics(client, name) for name in files}
            matched, differences = compare_diagnostics(diags, errors, files)
            for kind, detail, classification in differences:
                add(kind, detail, classification)
            if DUMP is not None:
                DUMP[f"{case.category}/{case.id}"] = {
                    file: sorted(diagnostic_range(d) for d in diagnostics) for file, diagnostics in diags.items()
                }
            for file, code, want in case.ranges:
                got = [(d.get("code"), diagnostic_range(d)) for d in diags.get(file, ())]
                if not any(span == want and (code is None or code == actual) for actual, span in got):
                    add("CORPUS_RANGE_MISMATCH", f"{file}: expected {code or 'error'} {want}, server {got}")

        if marker:
            _, _, line, character, _ = marker
            result = client.tool("completion", path="main.as", line=line, character=character, limit=500)
            items = [i for i in result["items"] if i.get("kind") not in (KIND_KEYWORD, KIND_SNIPPET)]
            labels = {i["label"] for i in items}
            for label in case.plus:
                if label not in labels:
                    add("COMPLETION_MISSING", label)
            for label in case.minus:
                if label in labels:
                    add("COMPLETION_LEAK", label)
            if case.scan_all:
                for label in sorted(labels):
                    if not label.lower().startswith(prefix.lower()):
                        continue
                    if not judge(label):
                        add(
                            "COMPLETION_LEAK", f"{label} (kind {next(i['kind'] for i in items if i['label'] == label)})"
                        )
    finally:
        if client is not None:
            client.close()
        shutil.rmtree(root, ignore_errors=True)
    return matched, findings


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", default=str(SERVER_DIR / "tests" / "parity" / "differential"))
    parser.add_argument("--lsp", default=str(SERVER_DIR / "build" / "Release" / "angel_lsp.exe"))
    parser.add_argument("--oracle", default=str(SERVER_DIR / "build" / "Release" / "angelscript_oracle.exe"))
    parser.add_argument("--log", default=str(SERVER_DIR / "build" / "lsp_discrepancies_log.json"))
    parser.add_argument("--filter", default="")
    parser.add_argument("--dump-ranges", default="", help="write the server's error ranges per case to this JSON file")
    parser.add_argument("--show-unpaired", action="store_true", help="print informational unpaired diagnostic details")
    args = parser.parse_args()
    global DUMP
    DUMP = {} if args.dump_ranges else None

    root = Path(args.cases)
    cases = []
    for category in sorted(p for p in root.iterdir() if p.is_dir()):
        for entry in sorted(category.iterdir()):
            if entry.is_dir() and (entry / "main.as").exists() or entry.suffix == ".as":
                cases.extend(c for c in Case.load(entry) if args.filter in f"{c.category}/{c.id}")
    with ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(lambda c: evaluate(c, Path(args.oracle), Path(args.lsp)), cases))
    findings = [finding for _, case_findings in results for finding in case_findings]
    for finding in findings:
        if finding["case"].startswith("_"):
            finding["kind"] = f"KNOWN_GAP_{finding['kind']}"
            finding["classification"] = "known_gap"
    kinds = Counter(finding["kind"] for finding in findings)
    classes = Counter(finding["classification"] for finding in findings)
    summary = {
        "cases": len(cases),
        "matched": sum(matched for matched, _ in results),
        "server_only": kinds["SERVER_ONLY"],
        "oracle_only": kinds["ORACLE_ONLY"],
        "range_file_mismatch": kinds["RANGE_FILE_MISMATCH"],
        "verdict_mismatch": kinds["VERDICT_MISMATCH"],
        "position_unpaired": classes["informational"] - kinds["SERVER_EXTRA_FILE"],
        "server_extra_files": kinds["SERVER_EXTRA_FILE"],
        "known_gaps": classes["known_gap"],
        "oracle_failures": kinds["ORACLE_ERROR"],
        "oracle_contradictions": kinds["ORACLE_CONTRADICTS_CASE"],
        "harness_errors": kinds["HARNESS_ERROR"],
    }
    summary["discrepancies"] = classes["failing"]
    if DUMP is not None:
        Path(args.dump_ranges).write_text(json.dumps(DUMP, indent=1), encoding="utf-8")
    Path(args.log).write_text(json.dumps({"summary": summary, "findings": findings}, indent=2), encoding="utf-8")
    print(" ".join(f"{name}={count}" for name, count in summary.items()))
    for f in findings:
        if f["classification"] == "informational" and not args.show_unpaired:
            continue
        print(f"  {f['kind']:<24} {f['case']}: {f['detail']}")
    if not cases:
        print("No differential cases matched.", file=sys.stderr)
    return int(bool(len(findings) - classes["informational"] - classes["known_gap"]) or not cases)


if __name__ == "__main__":
    sys.exit(main())
