# AngelScript Language Server

Language support for [AngelScript](https://www.angelcode.com/angelscript/) (`.as`), backed by a
native C++ language server that parses with Tree-sitter rather than by embedding the AngelScript
engine. No script concatenation, no engine callbacks — the whole workspace is analysed from its
syntax trees.

---

## Minimalist Quickstart

### 1. Install Extension

Install the extension from the VS Code Marketplace or install the packaged `.vsix` directly:
- Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on macOS) and run `Extensions: Install from VSIX...`.
- Select `angelscript-lsp-0.7.7-exp.8.vsix`.

### 2. Workspace Configuration

Open your workspace containing `.as` scripts. Create or update `.vscode/settings.json`:

```jsonc
{
  "angelscript.searchDirectories": [
    "${workspaceFolder}/scripts"
  ],
  "angelscript.predefinedFiles": [
    "${workspaceFolder}/stubs/as.predefined"
  ]
}
```

If your host engine supports implicit file extensions (such as Sven Co-op's `#include "helper"`), enable:

```jsonc
{
  "angelscript.include.implicitExtension": true
}
```

---

## Feature Status & Reliability Checklist

| Feature / LSP Method | Status | Reliability Notes | Configuration Flag |
| :--- | :--- | :--- | :--- |
| **Diagnostics**<br>`textDocument/publishDiagnostics` | Stable / Production-Ready | Dual-pass (syntax + semantic) validation oracle. Fast AST error detection with debounced background semantic pass. FNV-1a 64-bit ABI fingerprinting prevents cascading save storms when public interfaces are untouched. | `--enable-type-conversion-checks` |
| **Hover**<br>`textDocument/hover` | Stable / Production-Ready | Sub-millisecond keyed spatial lookup (zero linear table scans). Full Doxygen docstring parser (`@brief`, `@param`, `@return`, `@see`), property accessors, and virtual document host scope fallback. | `--enable-hover` |
| **Definition & Declaration**<br>`textDocument/definition`<br>`textDocument/declaration`<br>`textDocument/typeDefinition` | Stable / Production-Ready | Precise cross-file symbol lookup. Overload-aware callee argument scoring (`FilterOverloadsForCall`), mixin origin source mapping (jumps to template declaration range), and base/interface traversal. | `--enable-definition` |
| **References**<br>`textDocument/references` | Stable / Production-Ready | Strict scope isolation across inheritance boundaries: private members isolated to declaring class AST; protected restricted to derived classes; public resolved to highest declaring ancestor. Eliminates sibling class leakage. | `--enable-references` |
| **Rename**<br>`textDocument/prepareRename`<br>`textDocument/rename` | Stable / Production-Ready | Multi-file `WorkspaceEdit` generation. Safe identifier renaming protected against lexical shadowing and keyword collisions; guaranteed occurrence parity with Find References. | `--enable-rename` |
| **Completion**<br>`textDocument/completion` | Stable / Production-Ready | Scope-aware suggestions for locals, parameters, class members (`.`, `->`), namespace members (`::`), and keywords. Parameter placeholders and auto-expanding snippets for control structures. | `--enable-completion` |
| **Signature Help**<br>`textDocument/signatureHelp` | Stable / Production-Ready | Active parameter index tracking during call expressions. Overload candidate preview and associated documentation formatting. | `--enable-signature-help` |
| **Semantic Tokens**<br>`textDocument/semanticTokens/full`<br>`textDocument/semanticTokens/range` | Stable / Production-Ready | Zero-allocation delta integer streams with standard LSP legend. Distinguishes parameters, member properties, locals, and enum constants through symbol table resolution. Supports inactive preprocessor range dimming. | `--enable-semantic-tokens` |
| **Document Symbols**<br>`textDocument/documentSymbol` | Stable / Production-Ready | Hierarchical symbol tree (classes, methods, fields, enums, namespaces) powering the VS Code Outline view and breadcrumb navigation. | `--enable-document-symbols` |
| **Workspace Symbols**<br>`workspace/symbol` | Stable / Production-Ready | Multi-tiered fuzzy search, scoring, and ranking across all indexed project scripts and predefined host stubs (`Ctrl+T`). | `--enable-workspace-symbols` |
| **Inlay Hints**<br>`textDocument/inlayHint` | Stable / Production-Ready | Inline parameter name hints for standard calls, constructor direct-initializations, `BaseClass` methods, and utility objects. Configurable suppression when argument text matches parameter name. | `--enable-inlay-hints` |
| **CodeLens**<br>`textDocument/codeLens` | Stable / Production-Ready | Inline actionable reference counts above declarations. Deduplicates identical mixin declaration ranges and aggregates reference counts across synthesized host classes without leakage. | `angelscript.features.codeLens` |
| **Call Hierarchy**<br>`textDocument/prepareCallHierarchy`<br>`callHierarchy/incomingCalls`<br>`callHierarchy/outgoingCalls` | Stable / Production-Ready | Workspace-wide call indexing for functions, methods, and mixins. Synthesized host class caller methods resolve back to originating mixin bodies to locate inbound and outbound calls accurately. | `--enable-call-hierarchy` |
| **Type Hierarchy**<br>`textDocument/prepareTypeHierarchy`<br>`typeHierarchy/supertypes`<br>`typeHierarchy/subtypes` | Stable / Production-Ready | Bi-directional class and interface inheritance hierarchy exploration with strict LSP range containment verification. | `--enable-type-hierarchy` |
| **Formatting**<br>`textDocument/formatting`<br>`textDocument/rangeFormatting` | Stable / Production-Ready | Document and range formatting supporting Allman and K&R brace placement styles. Guaranteed token safety verified against 213 compiler parity test scripts. | `--enable-formatting` |
| **Virtual Mixin Documents**<br>`angelscript-virtual://<host>/<mixin>.as` | Experimental / Opt-in | Synthetic document provider enabling full AST mixin expansion. Native inline peek inspection (`angelscript.peekMixinInline`) and host-scope fallback for members like `self` and `m_pPlayer`. | `--enable-virtual-mixin-documents` |
| **Predefined Stubs**<br>Host API Loader (`as.predefined`) | Stable / Production-Ready | High-performance background loader bypassing diagnostic checker overhead (>95% speedup). Native `@listpattern` and `{repeat T}` initializer list support. Integrated stub consolidation formatter. | `--enable-predefined-loader` |
| **Module System**<br>Multi-Module Compilation | Stable / Production-Ready | Entry-point and folder-based module configurations. Dynamic hot-reloading via configuration changes, unconfigured closure cache purging, and strict validation of `external shared` declarations. | `angelscript.modules` |

---

## Settings

### Path variables

Every path-valued setting accepts the same `${...}` variables `launch.json` does. VS Code does not
expand these in ordinary settings, so the extension does it:

| Variable | Becomes |
| --- | --- |
| `${workspaceFolder}` | Each workspace folder. In a multi-root workspace an entry using it is resolved once per folder. |
| `${workspaceFolder:name}` | The folder with that name. |
| `${userHome}` | Your home directory. |
| `${env:NAME}` | An environment variable - the usual way a host SDK path is already written down. |

```jsonc
{
  "angelscript.predefined.active": "${workspaceFolder}/stubs/host.as.predefined",
  "angelscript.searchDirectories": ["${workspaceFolder}/scripts", "${env:SVENCOOP_SDK}/scripts"]
}
```

Absolute paths are used as-is, so the stub may live outside your workspace. Relative paths resolve
against each workspace folder.

A variable this window cannot answer - a folder name that does not exist, an unset environment
variable - is left in the path as written and noted in the server log, rather than silently
dropped.

The stub picker writes `${workspaceFolder}/...` when the stub you choose lives inside a workspace
folder, so the setting stays portable when it is committed. A stub outside every folder keeps its
absolute path, and the two can be mixed in one workspace.

| Setting | Default | What it does |
| --- | --- | --- |
| `angelscript.enableVirtualMixinDocuments` | `false` | Enables experimental virtual text documents for mixin classes (`angelscript-virtual://`). When disabled, high-performance symbol synthesis is used. |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Suppress parameter name hints when the argument expression text matches the parameter name exactly. |
| `angelscript.searchDirectories` | `[]` | Extra directories for resolving `#include "path.as"`. |
| `angelscript.predefined.active` | `""` | The one stub to load, when the workspace holds several. `"all"` merges them. |
| `angelscript.statusBar.alignment` | `left` | Which side of the status bar the AngelScript item sits on. |
| `angelscript.predefinedFiles` | `[]` | Stub files describing the host application's API, loaded by path. |
| `angelscript.predefinedExtension` | `.as.predefined` | Filename suffix that marks a workspace file as a stub. |
| `angelscript.include.implicitExtension` | `false` | Let `#include "helper"` find `helper.as`, for hosts that resolve the name themselves (Sven Co-op). |
| `angelscript.modules` | `[]` | The script modules this workspace builds, as `{ "name", "entry" }` or `{ "name", "folder" }`. Publishes diagnostics for every file in a module, and makes `external shared` checkable. |
| `angelscript.fileExtension` | `.as` | Filename suffix of script files, used when scanning the workspace. |
| `angelscript.diagnosticSeverity` | `{}` | Per-diagnostic severity overrides, e.g. `{"as-warn-unused-variable": "hint"}`. |
| `angelscript.features.*` | `true` | One switch per feature (hover, completion, formatting, …) if you want to turn one off. |

Changing any of these restarts the language server; there is no need to reload the window.

## License

MIT
