# Planned: a root script, and files opened from outside the workspace

Two requests that look separate and are the same question — *which files does this server consider
part of the program?* Neither is implemented. This is the case for implementing them, the design,
and the regressions each one can cause, written down before any code so the risky parts are argued
rather than discovered.

Nothing here is a promise of order or date.

---

## What the server does today

Measured against the code, not assumed:

**The include graph is built from workspace folders only.** `Server::ReadWorkspaceFiles` walks the
folders in `workspaceFolders`, and `WorkspaceIncludeGraph::Build` gets exactly those roots. A file
that lives outside every folder has no node in the graph.

**Includes are confined to a root allow-list.** `IncludeResolver::ResolveIncludePath` funnels every
successful resolution through one `permit()` lambda, and `Server::IncludeAllowedRoots` fills it with
the workspace folders, the configured `searchDirectories`, and the *parent directory of each
configured stub*. Confinement is a deliberate safety property, and the single exit point exists so a
new resolution strategy is confined by default.

**Opening a file is not gated on any of that.** `didOpen` parses, collects symbols and analyses
whatever it is handed.

Put together, that produces the behaviour worth naming:

> A `.as` opened from outside every workspace folder is analysed, but its `#include "sibling.as"`
> does not resolve. The resolver finds the file next to it and then refuses it, because its
> directory is not in the allow-list. Everything the sibling declares is reported as an unresolved
> type — legal code marked as errors, which is the one failure this project treats as fatal.

The same is true of an `.as.predefined` opened from outside: `PredefinedStubContributes` answers
"no" when a different stub is the active selection, so the file is read as a document and
contributes no host types.

---

## Idea 1 — a root script

**Ask:** mark one `.as` as the entry point, so the server stops loading scripts that are not part of
that program.

**Why it is worth doing.** A workspace can hold several programs that never see each other — a map
script directory where every file is its own entry point, a repository holding both a server
plugin and a tool. Today every one of them is indexed together, so completion offers names from
programs this file cannot reach and a duplicate-declaration rule can fire across two programs that
would never be compiled together. The include graph already knows the answer: `GetModuleClosure`
computes exactly the set of files an entry point pulls in.

**Design.** A setting, `angelscript.rootScript`, holding one path — the same variable expansion as
every other path setting, so `${workspaceFolder}/scripts/main.as` works.

When it is set:

- the workspace scan still builds the include graph over every folder, because the graph is what
  computes the closure and it is directives-only and cheap;
- but only the closure of the root script is *indexed* — symbols collected into the table;
- a file outside that closure still opens, parses, formats and navigates within itself. It stops
  contributing symbols to other files, and stops receiving theirs.

When it is unset, nothing changes. That is the default and it must stay the default.

**Regressions this can cause, and what each needs.**

| Risk | Why it bites | What it needs |
|---|---|---|
| A file the user is editing falls outside the closure and every host type in it stops resolving | This is the false-positive failure mode, in the exact shape this project treats as fatal. A user who sets a root script and then opens a map script gets a screen of errors on code that compiles | The status bar must say when a root script is in force, and a document outside the closure must be **silent**, not wrong — the same rule the analyzer already follows when it cannot see a type's world |
| The stubs stop loading | Stubs are not reachable from the root script by `#include`; they are the host's vocabulary, not part of the program | Stub loading is a separate path already (`LoadConfiguredPredefinedFiles`, `LoadBuiltinEngineProfiles`) and must stay outside the closure filter entirely |
| A root script that does not exist | The same silent-fallback failure this project has already been bitten by twice — an unknown engine profile name used to fall back to `standard` in silence | A path that resolves to nothing must be a visible message, not a fallback to "index everything" |
| Editing an `#include` changes the closure | A file can enter or leave the program as the user types | The closure has to be recomputed where `didSave` already patches the graph, and every open document re-diagnosed — the same fan-out the stub reload now does |

**How it would be proven.** A workspace with two programs that share no include: with no root
script both index, with the root script set only one does, and the other is *silent* rather than
full of errors. Three cases, and the third is the one that matters.

---

## Idea 2 — a script opened from outside the workspace

**Ask:** open a `.as` from anywhere and have its `#include` lines resolve from where that file is.

**Why it is worth doing.** It is already half-working, and the half that fails fails in the worst
way: the file analyses, so the user sees a confident screen of errors about code that compiles.
Refusing to analyse it at all would be more honest than what happens now.

**Design.** When a document is opened whose path is under no workspace folder and no search
directory, treat its own directory as a root **for that document only**:

- add the directory to the allow-list used while resolving *its* includes;
- index the closure reachable from it, as `IndexModuleClosure` already does for a workspace file;
- release all of it on `didClose`, which `ReleaseModuleClosure` already knows how to do.

The confinement is not being removed. It is being widened by one directory, for one open document,
for as long as it is open — and the user opening the file is the authorisation, the same way opening
a folder authorises that folder.

**Regressions this can cause, and what each needs.**

| Risk | Why it bites | What it needs |
|---|---|---|
| Confinement is weakened | `permit()` exists to stop `#include "../../../etc/passwd"` walking out of the workspace. Adding a root makes that root's tree reachable | The added root must be **that document's own directory**, never its parents, and must live only as long as the document is open. It must not enter `IncludeAllowedRoots()` globally, or one opened file would silently widen resolution for every other file |
| Two opened externals disagree | Each adds its own directory; a name resolvable from one becomes resolvable from the other | Per-document roots, keyed by document, not one shared list |
| An external file's symbols collide with the workspace's | Two `class Player` in one table is a duplicate-declaration error on code that compiles separately | The symbol table is already per-document with a URI key. The question is whether the *rules* should see both at once — and the honest answer is probably not: an external document is its own program until something says otherwise |
| It interacts with Idea 1 | If a root script is set, is an externally-opened file inside or outside the program? | Outside, and therefore silent. The two features have to be specified together, which is why they are in one document |
| An external `.as.predefined` | Opening a stub from outside does nothing today, and doing nothing is arguably right — but the user is not told | At minimum a message. Possibly an offer to add it to `predefinedFiles`, which is the action they almost certainly want |

**How it would be proven.** A fixture workspace and a second directory outside it, holding
`main.as` and `helper.as`. Opening the external `main.as` must resolve the type `helper.as`
declares — and the control is that a file in a *third* directory, not opened, must stay
unresolvable, or the test would pass against a server that had simply dropped the confinement.

---

## Why they are one document

If Idea 2 lands alone, an externally-opened file becomes part of the same index as the workspace,
and Idea 1 then has to decide what to do with it. If Idea 1 lands alone, the confinement bug in
Idea 2 stays. Both changes answer *which files are the program*, and the answer has to be one
answer.

The shape both share, and the one thing worth carrying forward regardless: **a file this server
has decided not to index must be silent about types it cannot see, never wrong about them.** That
rule already exists in the analyzer. Neither of these features may break it.
