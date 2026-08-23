# AngelScript Language Server

Language support for [AngelScript](https://www.angelcode.com/angelscript/) (`.as`), backed by a
native C++ language server that parses with Tree-sitter rather than by embedding the AngelScript
engine. No script concatenation, no engine callbacks — the whole workspace is analysed from its
syntax trees.

## Features

- **Hover** — full declarations with the modifiers the source actually wrote: `private`, `protected`,
  trailing `const`, `override`/`final`, handles (`@`) and parameter direction (`&in`, `&out`, `&inout`),
  plus the Doxygen comment above the declaration.
- **Navigation** — go to definition and type definition, find all references, and rename across files,
  each aware of class hierarchies and namespaces.
- **Completion** — scope-aware suggestions for locals, class members (inherited ones included),
  namespaces and globals. Documentation is attached when you highlight an item, not for all of them
  at once.
- **Semantic highlighting** — resolved through the scope tree, so a parameter, a field and a local
  are coloured as what they are rather than all as plain variables. Supports viewport-sized and
  incremental token requests.
- **Diagnostics** — syntax and semantic errors, including conversions with nothing to back them:
  a `T v = expr;` with no converting constructor, `opImplConv` or `opAssign`, a `T(expr)` with no
  matching constructor, or a `cast<T>(expr)` between unrelated types.
- **Editing** — document and range formatting (Allman braces, configurable indentation), quick fixes,
  folding ranges, inlay hints, document highlights and clickable `#include` links.
- **Workspace awareness** — an `#include` graph decides which files are indexed together, files
  changed outside the editor are picked up, and predefined stub files describing the host
  application's API are loaded from anywhere on disk.

## Getting started

Open a folder containing `.as` files. The server starts on its own.

If your host application ships an API stub (Sven Co-op's `as.predefined`, for example), point the
extension at it — this is what makes the engine's own types resolve:

```jsonc
{
  "angelscript.predefinedFiles": ["C:/Games/svencoop/svencoop/scripts/as.predefined"],
  "angelscript.searchDirectories": ["scripts", "scripts/maps"]
}
```

Absolute paths are used as-is, so the stub may live outside your workspace. Relative paths resolve
against each workspace folder.

## Settings

| Setting | Default | What it does |
| --- | --- | --- |
| `angelscript.searchDirectories` | `[]` | Extra directories for resolving `#include "path.as"`. |
| `angelscript.predefinedFiles` | `[]` | Stub files describing the host application's API, loaded by path. |
| `angelscript.predefinedExtension` | `.as.predefined` | Filename suffix that marks a workspace file as a stub. |
| `angelscript.fileExtension` | `.as` | Filename suffix of script files, used when scanning the workspace. |
| `angelscript.diagnosticSeverity` | `{}` | Per-diagnostic severity overrides, e.g. `{"as-warn-unused-variable": "hint"}`. |
| `angelscript.features.*` | `true` | One switch per feature (hover, completion, formatting, …) if you want to turn one off. |

Changing any of these restarts the language server; there is no need to reload the window.

## License

MIT
