# Angelscript - Language Server for Angelscript (VS Code Extension)

Angelscript provides rich language intelligence for [AngelScript](https://www.angelcode.com/angelscript/) (`.as`), powered by a native C++20 language server using Tree-Sitter for AST parsing and semantic resolution. The entire workspace is analyzed directly from syntax trees without script concatenation, intermediate disk dumps, or host engine execution.

---

## Quickstart

### 1. Installation

Install via the [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman) or search for `Angelscript` by Gaftherman in the Extensions view (`Ctrl+Shift+X`):
1. Open Visual Studio Code.
2. Press `Ctrl+P`, paste `ext install Gaftherman.angelscript-gaftherman`, and press Enter.

Alternatively, to install from a packaged `.vsix` bundle:
1. Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on macOS) and run `Extensions: Install from VSIX...`.
2. Select the compiled extension package (`angelscript.vsix` or `angelscript-*.vsix`).

### 2. Workspace Setup

Open your workspace folder in VS Code. Configure `.vscode/settings.json` according to your engine setup:

#### Example A: Sven Co-op (Folder Module)
Recommended for Sven Co-op map script folders (e.g. `maps/hcas`):
```jsonc
{
  // Allow extensionless includes (e.g. #include "helper" resolves to helper.as)
  "angelscript.include.implicitExtension": true,

  // Extra search root for relative and angular-bracket #include paths
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],

  // Folder module ownership (all scripts under maps/hcas belong to module HCAS)
  "angelscript.modules": [
    {
      "name": "HCAS",
      "folder": "${workspaceFolder}/maps/hcas"
    }
  ],

  // Enable virtual mixin document inspection (angelscript-virtual://)
  "angelscript.enableVirtualMixinDocuments": true,

  // Active predefined host stub
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

#### Example B: Sven Co-op (Entry Script Module)
Recommended when compiling a script tree starting from a registration entry point:
```jsonc
{
  "angelscript.include.implicitExtension": true,
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],
  "angelscript.modules": [
    {
      "name": "MapInit",
      "entry": "${workspaceFolder}/maps/ins2/ins2_register.as"
    }
  ],
  "angelscript.enableVirtualMixinDocuments": true,
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

#### Example C: Generic Game Engine / Standalone Workspace
For standalone AngelScript host integrations using custom include paths and API stubs:
```jsonc
{
  "angelscript.searchDirectories": [
    "${workspaceFolder}/scripts/include"
  ],
  "angelscript.modules": [
    {
      "name": "GameCore",
      "folder": "${workspaceFolder}/scripts/game"
    }
  ],
  "angelscript.predefinedFiles": [
    "${workspaceFolder}/scripts/api/engine.as.predefined"
  ]
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
- **Precise Hover**: Overload-isolated hover at call sites, Doxygen docstrings (`@brief`, `@param`, `@return`), direct-initialization constructor resolution, string literal asset status and file metrics, and anonymous lambda contracts.
- **Navigation & Go-to-Definition**: Precise symbol jump across files and stubs with overload argument matching (`FilterOverloadsForCall`), mixin origin mapping, and interface implementation discovery (`Ctrl+F12`).
- **Intelligent Autocompletion**: Scope-aware member completions (`.`), namespace lookups (`::`), smart type-aware ranking for call arguments and assignment expressions, and control-flow snippet expansions.
- **Semantic Highlighting**: Zero-allocation delta integer streams with full standard LSP token classification distinguishing parameters, members, locals, types, bare enum constants, and inactive preprocessor branches.
- **Inlay Hints**: Inline parameter name hints with full multi-part argument range highlighting, omitted default parameter values, fallback types for wildcard parameters, and configurable suppression.
- **CodeLens & Call Hierarchy**: Cross-file and cross-namespace reference counts above declarations, virtual property accessor tracking, and full bi-directional call tree indexing (`textDocument/prepareCallHierarchy`).
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
| `angelscript.inlayHints.maxParameters` | `0` | Maximum number of parameter inlay hints to display per call (`0` = unlimited). |
| `angelscript.inlayHints.maxLength` | `0` | Maximum character length for parameter inlay hint labels before truncating with `...` (`0` = unlimited). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Suppresses parameter name hints when argument text matches parameter name. |
| `angelscript.inlayHints.omittedDefaultArguments` | `"nameAndValue"` | Inlay hints for omitted default arguments (`"nameAndValue"`, `"declaration"`, `"off"`). |
| `angelscript.completion.smartTypeRanking` | `true` | Contextual type ranking prioritizing matching parameter and assignment types. |
| `angelscript.statusBar.alignment` | `"left"` | Alignment of the AngelScript status bar item (`"left"` or `"right"`). |
| `angelscript.diagnosticSeverity` | `{}` | Per-diagnostic severity overrides (e.g. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.engine.requireEnumScope` | `false` | When true (`asEP_REQUIRE_ENUM_SCOPE`), enums must be qualified with `Enum::Member`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | When true (`asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`), default constructor is always synthesized. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | When true (`asEP_IGNORE_DUPLICATE_SHARED_INTF`), identical shared interfaces across files are ignored. |
| `angelscript.features.*` | `true` | Individual toggles for LSP features (hover, completion, formatting, etc.). |
| `angelscript.format.braceStyle` | `"allman"` | Brace placement style (`"allman"` or `"kr"`). |
| `angelscript.format.spacesInsideParentheses` | `false` | Whether to insert spaces inside parentheses (e.g. `foo( bar )` instead of `foo(bar)`). |

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
