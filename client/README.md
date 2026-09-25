# Angelscript - Language Server for Angelscript (VS Code Extension)

Angelscript provides rich language intelligence for [AngelScript](https://www.angelcode.com/angelscript/) (`.as`), powered by a native C++20 language server using Tree-Sitter for AST parsing and semantic resolution. The entire workspace is analyzed directly from syntax trees without script concatenation, intermediate disk dumps, or host engine execution.

---

## Quickstart

### 1. Installation

Install via the Visual Studio Code Marketplace or from a packaged `.vsix` bundle:
1. Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on macOS) and run `Extensions: Install from VSIX...`.
2. Select the compiled extension package (`angelscript.vsix` or `angelscript-0.8.5.vsix`).

### 2. Workspace Setup

Open your workspace folder in VS Code. Configure your `.vscode/settings.json`:

```jsonc
{
  // Search paths for #include resolution
  "angelscript.searchDirectories": [
    "${workspaceFolder}/scripts"
  ],

  // Load host engine API stub definitions
  "angelscript.predefinedFiles": [
    "${workspaceFolder}/stubs/sven.as.predefined"
  ],

  // Enable extensionless include resolution (e.g. #include "helper" finds "helper.as")
  "angelscript.include.implicitExtension": true
}
```

---

## Internationalization & Localization (i18n / l10n)

AngelLSP provides seamless, out-of-the-box bilingual localization in both **English** and **Spanish**:

- **Automatic Language Sync**: The extension automatically adapts to your VS Code display language (`Configure Display Language` in the Command Palette).
- **Extension UI & Settings**: All 112 configuration settings, command titles, status bar items, and notification dialogs are natively localized via `@vscode/l10n` (`bundle.l10n.json` and `bundle.l10n.es.json`) and manifest NLS tables (`package.nls.json` and `package.nls.es.json`).
- **Server Diagnostics**: The language server forwards diagnostic messages in the active locale, ensuring compiler errors and hover descriptions match your language preference.
- **Manual Locale Override**: You can explicitly select your language by configuring the server startup argument or passing `--locale=es` / `--locale=en`.

---

## Key Features

- **Semantic Diagnostics**: Real-time syntax and semantic validation with debounced background passes and FNV-1a ABI fingerprinting to prevent cascading analysis storms on saved documents.
- **Flow-Sensitive Null Checks**: Intraprocedural null handle dereference diagnostics (`as-warn-possible-null-dereference`) warning on unchecked handles or handles used after null assignment.
- **Precise Hover**: Overload-isolated hover at call sites, Doxygen docstring rendering (`@brief`, `@param`, `@return`), and anonymous lambda resolution displaying target `funcdef` signatures and contracts.
- **Navigation & Go-to-Definition**: Precise symbol jump across files and stubs with overload argument matching (`FilterOverloadsForCall`), mixin origin mapping, and interface implementation discovery (`Ctrl+F12`).
- **Intelligent Autocompletion**: Scope-aware member completions (`.`, `->`), namespace lookups (`::`), and control-flow snippet expansions.
- **Semantic Highlighting**: Zero-allocation delta integer streams with full standard LSP token classification distinguishing parameters, members, locals, types, and inactive preprocessor branches.
- **Inlay Hints**: Inline parameter name hints with type deduction on nested calls and configurable suppression when argument names match formal parameters.
- **CodeLens & Call Hierarchy**: Reference counts above declarations and full bi-directional call tree indexing (`textDocument/prepareCallHierarchy`).
- **Document & Workspace Symbols**: Hierarchical symbol outlines for breadcrumbs and outline views, plus fuzzy workspace-wide symbol search (`Ctrl+T`).
- **Virtual Mixin Documents**: Synthetic document inspection (`angelscript-virtual://`) enabling inline peek and host-scoped member validation.

---

## Configuration Reference

### Path Variables

Path-valued settings support dynamic variable expansions matching VS Code's `launch.json` standard:

| Variable | Expands To |
| :--- | :--- |
| `${workspaceFolder}` | The root directory of the active workspace folder. |
| `${workspaceFolder:name}` | The root directory of the named workspace folder in a multi-root workspace. |
| `${userHome}` | The current user's home directory. |
| `${env:NAME}` | Value of the environment variable `NAME` (e.g. `${env:SVENCOOP_DIR}`). |

### Settings Catalog

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.searchDirectories` | `[]` | Extra directories to scan for `#include "path.as"` resolution. |
| `angelscript.include.implicitExtension` | `false` | Allows `#include "helper"` to resolve to `helper.as` without requiring the extension. |
| `angelscript.predefinedFiles` | `[]` | List of predefined host API stub files (`.as.predefined`). |
| `angelscript.predefined.active` | `""` | The active stub to load when multiple are present. Set to `"all"` to merge all stubs. |
| `angelscript.predefinedExtension` | `.as.predefined` | Suffix identifying workspace stub files. |
| `angelscript.modules` | `[]` | Script module definitions specified as `{"name", "entry"}` or `{"name", "folder"}`. |
| `angelscript.fileExtension` | `.as` | Suffix of script files scanned in the workspace. |
| `angelscript.enableVirtualMixinDocuments` | `false` | Enables virtual document providers for mixin class expansion. |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Suppresses parameter name hints when argument text matches parameter name. |
| `angelscript.statusBar.alignment` | `"left"` | Alignment of the AngelScript status bar item (`"left"` or `"right"`). |
| `angelscript.diagnosticSeverity` | `{}` | Per-diagnostic severity overrides (e.g. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.features.*` | `true` | Individual toggles for LSP features (hover, completion, formatting, etc.). |
| `angelscript.format.braceStyle` | `"allman"` | Brace placement style (`"allman"` or `"kr"`). |

Settings modifications are dynamically applied without requiring a VS Code window reload.

---

## Workspace Trust & Security

AngelLSP implements strict security boundaries under VS Code's Workspace Trust model:
- In **Untrusted Workspaces**, custom server executable paths configured in workspace settings (`server.executablePath`) are strictly disabled and ignored.
- Only the bundled language server binary or global user settings may be used, protecting against remote code execution via untrusted repository configuration.

---

## Acknowledgements & Credits

- **AngelScript Logo & Brand**: The official AngelScript icon is adapted from the [AngelScript website](https://www.angelcode.com/angelscript/) by Andreas Jönsson.
- **File Icons (`.as` / `.as.predefined`)**: Sourced from the wonderful [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (specifically the ActionScript icon), temporarily borrowed for testing while custom AngelScript icons are being designed—all credit and thanks to Philipp Kief and the Material Icon Theme contributors :P.

---

## License

This project is licensed under the MIT License. See the [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) file for details.
