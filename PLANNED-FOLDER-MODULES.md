# Planned: folder modules, and module-wide diagnostics

`angelscript.modules` exists and takes `{ name, entry }` — a module named after the `.as` the host
builds it from, everything that file reaches through `#include` belonging to it. This is the design
for the two things it does not do yet:

- a **folder** as a module, so `scripts/maps/` is `MapScript` and `scripts/plugins/` is `Plugin`
  without naming an entry point for either;
- **module-wide diagnostics**, so an error in a file the entry point includes shows up in the
  Problems panel and can be clicked through, instead of appearing only once you open that file.

Not implemented. The three decisions below were put to the user and answered; everything else here
is a recommendation with its reasoning, and the last section is the part worth reading before any
of it is built.

---

## Where the shape comes from

Sven Co-op, which is the host this is being designed against:

```
svencoop/scripts/maps/      -> module "MapScript"
svencoop/scripts/plugins/   -> module "Plugin"
```

A directory decides which module a script belongs to. Other hosts do the same thing with their own
paths. That is the whole feature: a folder, a name, and everything under it.

---

## Decided

**A file belongs to exactly one module, and the most specific claim wins.**

```
entry point named explicitly   >   deepest folder module   >   any folder module above it
```

So with `map/` as `Map` and `map/anotherFolder/` as `Map-Script`, a file in `anotherFolder`
belongs to `Map-Script`. The nesting question answers itself: the inner declaration is more
specific, so it wins, the same way a nested `.gitignore` or `.editorconfig` does.

AngelScript really does allow one file to be compiled into several modules, so this is a
simplification. **When a file is claimed by more than one, the server says so** — a hint on the
file naming the module it was assigned to and the ones it also matched. Silence there would make
the rule invisible exactly when it matters.

**A folder module may also name an entry point.** `{ name, folder, entry? }`. The folder decides
membership; the entry point, when present, is what module-wide analysis starts from. Different
modules have their own entry points, independently.

**The whole module is re-analysed on save, and when the workspace is scanned.** Typing re-analyses
the open document only, exactly as today. An error introduced in `A.as` that breaks `B.as` appears
in `B.as` when `A.as` is saved. The alternative — the whole module after every typing pause — is a
promise that cannot be made before it is measured on a module of a few hundred files.

**Diagnostics are published for every file in the module, and withdrawn when a file leaves it.**
Publishing for an unopened file is what makes the Problems panel useful; not withdrawing is what
leaves ghosts in it after a module is renamed or removed. Both halves or neither.

---

## Settings

```jsonc
{
  "angelscript.modules": [
    { "name": "MapScript", "folder": "${workspaceFolder}/scripts/maps" },
    { "name": "Plugin",    "folder": "${workspaceFolder}/scripts/plugins" },
    { "name": "Core",      "entry":  "${workspaceFolder}/scripts/core/main.as" },
    { "name": "Editor",    "folder": "${workspaceFolder}/tools/editor",
                           "entry":  "${workspaceFolder}/tools/editor/main.as" }
  ]
}
```

One array, as now. An entry needs a `name` and at least one of `folder` or `entry`. Both is the
fourth case: the folder decides who is in, the entry decides where analysis starts.

Two commands, on the explorer context menu, which is what was asked for:

- on a `.as` — **Set as module entry point**, prompting for a module name, defaulting to the file's
  own stem;
- on a folder — **Set as module folder**, prompting for a name, defaulting to the folder's name.

Both write into `angelscript.modules` in workspace settings, through the same `${workspaceFolder}`
rewriting the stub picker already does, so the setting stays portable when it is committed.

---

## What has to be built

1. **Config** — `folder` alongside `entry`, and the pair validated: a `name` that is empty, a
   `folder` that is not a directory, an `entry` that is not a file. Each of those is a message, not
   a silent skip. A module that resolves to nothing must never look like a module with nothing to
   say.

2. **Membership** — one function answering "which module owns this path", implementing the
   precedence above. Everything else reads it. This is the piece most likely to grow a second,
   subtly different copy, so it needs to be the only one from the start.

3. **The ambiguity hint** — computed where membership is, reported once per file.

4. **Module-wide analysis** — on save and on scan: for each file in the module, analyse and publish.
   Needs a record of what was published, per module, so it can be withdrawn.

5. **Withdrawal** — publish an empty list for every URI that was published for a module and is no
   longer in it. Triggered by: the module's definition changing, the module being removed, a file
   being deleted, and an `#include` edit that shrinks an entry point's closure.

6. **The two commands** and their menu contributions.

---

## What can go wrong

The list that matters. Each of these is a case to write a test for, not a worry to keep in mind.

| Case | What happens without care |
|---|---|
| **A file is claimed by two modules** | Two sets of diagnostics for one file, alternating depending on which analysis ran last. The precedence rule plus the hint is the answer, and the hint is what stops it being invisible. |
| **A module is removed from settings** | Every diagnostic it published stays in the Problems panel for the rest of the session. Withdrawal has to be driven by the settings change, not only by file events. |
| **A file is deleted while in a module** | Same, and the file cannot be opened to clear it by hand. |
| **The open document is also in a module** | Two publishers for one URI - the open-document path and the module pass - racing, and the loser's answer is whatever was computed from the older text. One publisher per URI, and the open document's own analysis must win. |
| **A folder module contains another module's entry point** | Legal and probably intentional. Precedence decides it; the hint says so. |
| **A module folder outside every workspace folder** | The include resolver confines resolution to workspace folders and search directories. A module folder is an explicit statement that this directory is part of the project, so it has to join that allow-list - and that is a widening of a security boundary, so it must be deliberate rather than incidental. |
| **Two modules with the same name** | The `external shared` rule asks "declared in another module", which becomes meaningless. Reject at configuration time with a message. |
| **A module of several hundred files** | Analysing all of it on every save is the cost this design accepts. It has to be measured before it ships, on the shape of workspace it is aimed at, and the answer belongs in the doc comment the way the startup numbers now are. |
| **Case-only differences on Windows and macOS** | `scripts/Maps` and `scripts/maps` are one folder there and two on Linux. Membership compares paths, so it has to compare them the way the rest of this server does - `PathsAreSameFile`, not `==`. |
| **A symlinked folder inside a module folder** | Can put one file under two module roots, and can make a module contain itself. The walk needs the same cycle guard the include graph already has. |
| **The entry point of a folder module is outside that folder** | Allowed - the fourth settings case - but membership and analysis have to agree on what happens to files the entry point includes from outside the folder. Recommendation: they are in the module, because the compiler would compile them into it. |
| **Renaming a module folder on disk** | Stale membership plus stale diagnostics. This is where the path cache added for startup could hand back the old directory too, so the two need to be invalidated together. |

---

## What this does not change

Everything above is inert until `angelscript.modules` names something. With it empty the server
behaves exactly as it does today: the open document is analysed, its include closure is indexed,
and `external shared` asks the older, laxer question. That is the property to protect while
building this, and the reason the existing module tests keep a case for the unconfigured workspace.
