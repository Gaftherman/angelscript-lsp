#!/usr/bin/env python3
"""Run against the bts_rc corpus with the workspace root containing scripts/ and as.predefined.

Usage: python server/tests/tools/check_bts_references.py <server> <workspace-root>
"""

import argparse
import json
from pathlib import Path
from queue import Queue
import subprocess
from threading import Thread
from urllib.parse import unquote


BTS = "maps/bts_rc/"
WEAPONS = BTS + "entities/weapons/"
# Source lines are one-based. The start and exclusive end columns are UTF-16.
EXPECTED = {
    "ColorRed": (
        "mikk155/TextMenu/v1.as", 142,
        [
            (BTS + "entities/env_credits.as", 382, 30, 38),
            (BTS + "gamemodes/PlayerClass.as", 449, 23, 31),
        ],
    ),
    "Hit": (
        WEAPONS + "base/BTS_MeleeWeapon.as", 32,
        [
            (WEAPONS + "Melee/axe.as", 113, 25, 28),
            (WEAPONS + "Melee/broom.as", 102, 25, 28),
            (WEAPONS + "Melee/knife.as", 120, 28, 31),
            (WEAPONS + "Melee/knife.as", 151, 28, 31),
            (WEAPONS + "Melee/pipe.as", 102, 28, 31),
            (WEAPONS + "Melee/pipe.as", 132, 28, 31),
            (WEAPONS + "Melee/pipewrench.as", 108, 28, 31),
            (WEAPONS + "Melee/pipewrench.as", 159, 28, 31),
            (WEAPONS + "Melee/poolstick.as", 111, 25, 28),
            (WEAPONS + "Melee/screwdriver.as", 98, 25, 28),
            (WEAPONS + "Melee/spanner.as", 97, 25, 28),
            (WEAPONS + "Special/flashlight.as", 139, 25, 28),
        ],
    ),
}


class LspSession:
    def __init__(self, process):
        self.process = process
        self.messages = Queue()
        self.next_id = 1
        Thread(target=self.read, daemon=True).start()

    def read(self):
        try:
            stream = self.process.stdout
            while header := stream.readline():
                size = int(header.split(b":", 1)[1])
                while stream.readline().strip():
                    pass
                self.messages.put(json.loads(stream.read(size)))
            self.messages.put(EOFError("Server closed its output"))
        except (ValueError, OSError) as error:
            self.messages.put(error)

    def send(self, **message):
        data = json.dumps(dict(jsonrpc="2.0", **message)).encode()
        self.process.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
        self.process.stdin.flush()

    def wait_for(self, predicate):
        while True:
            message = self.messages.get(timeout=40)
            if isinstance(message, Exception):
                raise message
            if "id" in message and "method" in message:
                self.send(id=message["id"], result=None)
            if predicate(message):
                return message

    def request(self, method, params=None):
        request_id = self.next_id
        self.next_id += 1
        self.send(id=request_id, method=method, params=params or {})
        reply = self.wait_for(lambda item: item.get("id") == request_id and "method" not in item)
        if "error" in reply:
            raise RuntimeError(reply["error"])
        return reply.get("result")


def location_key(item):
    start = item["range"]["start"]
    end = item["range"]["end"]
    return (unquote(item["uri"]), start["line"], start["character"], end["line"], end["character"])


def check_declaration(session, scripts, name):
    path, row, uses = EXPECTED[name]
    declaration = scripts / path
    uri = declaration.as_uri()
    code = declaration.read_text(encoding="utf-8-sig")
    line = code.splitlines()[row - 1]
    column = len(line[:line.index(name)].encode("utf-16-le")) // 2
    doc = {"uri": uri}
    session.send(method="textDocument/didOpen", params={
        "textDocument": {**doc, "languageId": "angelscript", "version": 1, "text": code},
    })
    # Only the declaring document is open. Each use must come from the workspace index.
    results = session.request("textDocument/references", {
        "textDocument": doc,
        "position": {"line": row - 1, "character": column},
        "context": {"includeDeclaration": False},
    }) or []
    expected = sorted((unquote((scripts / path).as_uri()), line - 1, start, line - 1, end)
                      for path, line, start, end in uses)
    actual = sorted(map(location_key, results))
    if actual != expected:
        raise AssertionError(f"{name}: expected {expected!r}, received {actual!r}")

    lenses = session.request("textDocument/codeLens", {"textDocument": doc}) or []
    titles = [lens.get("command", {}).get("title") for lens in lenses
              if lens["range"]["start"]["line"] == row - 1]
    if titles != [f"{len(uses)} references"]:
        raise AssertionError(f"{name}: unexpected CodeLens titles {titles!r}")
    session.send(method="textDocument/didClose", params={"textDocument": doc})
    print(f"{name}: {len(uses)} exact UTF-16 locations with matching CodeLens")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=Path)
    parser.add_argument("workspace", type=Path)
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    scripts = workspace / "scripts"
    entry = scripts / BTS / "main.as"
    stub = workspace / "as.predefined"
    if not entry.is_file() or not stub.is_file():
        parser.error("workspace must contain scripts/maps/bts_rc/main.as and as.predefined")

    process = subprocess.Popen([
        str(args.server.resolve()), "--enable-code-lens", "--disable-file-logging", "--log-level=error",
        "--implicit-include-extension=true", f"--module=bts_rc={entry}", f"--predefined-file={stub}",
        "-DSERVER", "-DEDITOR_ONLY", "-DMETAMOD_PLUGIN_ASLP",
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        session = LspSession(process)
        root_uri = workspace.as_uri()
        session.request("initialize", {
            "processId": None,
            "rootUri": root_uri,
            "workspaceFolders": [{"name": "bts_rc", "uri": root_uri}],
            "capabilities": {"window": {"workDoneProgress": True}},
        })
        session.send(method="initialized", params={})
        session.wait_for(lambda item: item.get("method") == "$/progress"
                         and item["params"]["value"].get("kind") == "end")
        for name in EXPECTED:
            check_declaration(session, scripts, name)
        session.request("shutdown")
        session.send(method="exit")
        if process.wait(timeout=5) != 0:
            raise RuntimeError(f"Server exited with {process.returncode}")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
