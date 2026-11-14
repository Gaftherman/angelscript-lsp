#!/usr/bin/env python3
"""Differential validation of the language server against the AngelScript oracle.

Every case lives in server/tests/parity/differential/<category>/ as either `<id>.as` or a directory
`<id>/main.as` plus sibling files (reached through #include). Header comments in the main file:

    // @expect: accept|reject          what the real compiler must say about the script
    // @complete+ Label                 Label must compile when it replaces the marked prefix
    // @complete- Label                 Label must NOT compile there
    // @scan: all                       every non-keyword completion the server returns must compile

`/*$complete*/` marks the cursor and sits right after the partial identifier, so replacing that
identifier by a candidate yields a script the oracle can judge. The oracle runs first: a case whose
header disagrees with it is reported as ORACLE_CONTRADICTS_CASE and never reaches the server. Only
confirmed cases are replayed against `angel_lsp --mcp`, and each disagreement is written to the log.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parent.parent
MARKER = "/*$complete*/"
KIND_KEYWORD, KIND_SNIPPET = 14, 15


class Case:
    def __init__(self, path: Path):
        self.path = path
        self.main = path / "main.as" if path.is_dir() else path
        self.id = path.name if path.is_dir() else path.stem
        self.category = (path.parent).name
        self.text = self.main.read_text(encoding="utf-8")
        self.expect = None
        self.plus, self.minus, self.scan_all = [], [], False
        for line in self.text.splitlines():
            m = re.match(r"\s*//\s*@(expect|scan):\s*(\w+)", line)
            if m:
                if m.group(1) == "expect":
                    self.expect = m.group(2)
                else:
                    self.scan_all = m.group(2) == "all"
            m = re.match(r"\s*//\s*@complete([+-])\s*(\S+)", line)
            if m:
                (self.plus if m.group(1) == "+" else self.minus).append(m.group(2))

    def siblings(self):
        if not self.path.is_dir():
            return {}
        return {p.name: p.read_text(encoding="utf-8") for p in self.path.iterdir() if p.suffix == ".as" and p.name != "main.as"}

    def marker(self):
        """Returns (text without marker, prefix, line, character, offset) or None."""
        index = self.text.find(MARKER)
        if index < 0:
            return None
        before = self.text[:index]
        m = re.search(r"[A-Za-z_]\w*$|$", before)
        prefix = m.group(0)
        clean = before + self.text[index + len(MARKER):]
        line = before.count("\n")
        character = len(before.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2
        return clean, prefix, line, character, index


def run_oracle(oracle: Path, files: dict, main: str):
    """Compiles `main` in a scratch directory; returns (accepted, [(row, col, message)])."""
    work = Path(tempfile.mkdtemp(prefix="diff_oracle_"))
    try:
        for name, text in files.items():
            (work / name).write_text(text, encoding="utf-8")
        proc = subprocess.run([str(oracle), str(work / main), "--json", "--no-pause", "--no-mixin-probe"],
                              capture_output=True, text=True, timeout=60, cwd=work)
        try:
            report = json.loads(proc.stdout)
        except json.JSONDecodeError:
            return False, [(0, 0, "oracle output unreadable: " + proc.stdout[:120])]
        errors = [(m["row"], m["col"], m["message"]) for m in report["messages"] if m["type"] == "ERROR"]
        return (not report["hasError"]), errors
    finally:
        shutil.rmtree(work, ignore_errors=True)


class McpClient:
    def __init__(self, exe: Path, root: Path):
        self.proc = subprocess.Popen([str(exe), "--mcp", "--mcp-root", str(root)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.n = 0
        self._rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": "differential", "version": "1"}})
        self._rpc("notifications/initialized", notify=True)

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
        reply = json.loads(self.proc.stdout.readline())
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
        except Exception:
            self.proc.kill()


def evaluate(case: Case, oracle: Path, lsp_exe: Path):
    findings = []

    def add(kind, detail):
        findings.append({"case": f"{case.category}/{case.id}", "kind": kind, "detail": detail})

    files = {"main.as": case.text.replace(MARKER, "")}
    files.update(case.siblings())
    marker = case.marker()
    if marker:
        files["main.as"] = marker[0]

    # With a marker the text is a half-typed script, so the compiler can only judge the completed
    # forms below; diagnostics are compared only for cases without a marker.
    accepted, errors = (True, []) if marker else run_oracle(oracle, files, "main.as")
    if not marker and case.expect and accepted != (case.expect == "accept"):
        add("ORACLE_CONTRADICTS_CASE", f"header says {case.expect}, oracle {'accepts' if accepted else 'rejects'}: {errors[:1]}")
        return findings

    # Candidate verdicts from the oracle, decided before the server is ever asked.
    verdict = {}
    if marker:
        clean, prefix, _, _, _ = marker
        stem = clean[: marker[4] - len(prefix)]
        tail = clean[marker[4]:]
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
            return findings

    root = Path(tempfile.mkdtemp(prefix="diff_lsp_"))
    for name, text in files.items():  # a real client has the files on disk, which #include relies on
        (root / name).write_text(text, encoding="utf-8")
    client = McpClient(lsp_exe, root)
    try:
        for name, text in files.items():
            client.tool("open_document", path=name, text=text)
        diags = [] if marker else [d for d in client.tool("diagnostics", path="main.as") if d.get("severity") == 1]
        if marker:
            pass
        elif accepted and diags:
            add("FALSE_POSITIVE", f"line {diags[0]['range']['start']['line']}: {diags[0]['message'][:100]}")
        elif not accepted and not diags:
            add("FALSE_NEGATIVE", f"oracle: {errors[:1]}")
        elif not accepted:
            oracle_line = min(r for r, _, _ in errors if r > 0) - 1
            lsp_line = min(d["range"]["start"]["line"] for d in diags)
            if oracle_line != lsp_line:
                add("SPAN_MISMATCH", f"oracle line {oracle_line}, server line {lsp_line}: {diags[0]['message'][:80]}")

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
                        add("COMPLETION_LEAK", f"{label} (kind {next(i['kind'] for i in items if i['label'] == label)})")
    except Exception as error:  # noqa: BLE001 - a crashed server is itself a finding
        add("SERVER_ERROR", str(error)[:200])
    finally:
        client.close()
        shutil.rmtree(root, ignore_errors=True)
    return findings


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", default=str(SERVER_DIR / "tests" / "parity" / "differential"))
    parser.add_argument("--lsp", default=str(SERVER_DIR / "build" / "Release" / "angel_lsp.exe"))
    parser.add_argument("--oracle", default=str(SERVER_DIR / "build" / "Release" / "angelscript_oracle.exe"))
    parser.add_argument("--log", default=str(SERVER_DIR / "build" / "lsp_discrepancies_log.json"))
    parser.add_argument("--filter", default="")
    args = parser.parse_args()

    root = Path(args.cases)
    cases = []
    for category in sorted(p for p in root.iterdir() if p.is_dir()):
        for entry in sorted(category.iterdir()):
            if entry.is_dir() and (entry / "main.as").exists() or entry.suffix == ".as":
                case = Case(entry)
                if args.filter in f"{case.category}/{case.id}":
                    cases.append(case)

    with ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(lambda c: evaluate(c, Path(args.oracle), Path(args.lsp)), cases))
    findings = [f for r in results for f in r]
    Path(args.log).write_text(json.dumps(findings, indent=2), encoding="utf-8")

    contradictions = [f for f in findings if f["kind"] == "ORACLE_CONTRADICTS_CASE"]
    real = [f for f in findings if f["kind"] != "ORACLE_CONTRADICTS_CASE" and not f["case"].startswith("_")]
    print(f"cases={len(cases)} oracle_contradictions={len(contradictions)} discrepancies={len(real)}")
    for f in findings:
        print(f"  {f['kind']:<24} {f['case']}: {f['detail']}")
    return 1 if real else 0


if __name__ == "__main__":
    sys.exit(main())
