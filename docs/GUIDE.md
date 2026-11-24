# Angelscript Detailed Guide

[English](GUIDE.md) | [Español](GUIDE.es.md) | [README](../README.md)

> **Words you will see**
>
> - **Host** is the game or program that runs your scripts.
> - The **API** is the set of names the host supplies. A **stub** lists them in a file such as `game.as.predefined`.
> - **Entry module** groups a starting script, such as `main.as`, with every script it reaches through `#include`.
> - **Include** is a directive such as `#include "helper.as"` that brings another file into the module.
> - **Diagnostic** is a message in the editor, such as a warning about an unused variable.

## Workspace Setup Examples

### Start with one file

```text
my-project/
└── main.as
```

Open `my-project` in VS Code and put `int main() { return 42; }` in `main.as`. Hover over `main` to see `int main()`. No workspace settings or stubs are needed.

Put the engine example that matches your project in `.vscode/settings.json` under `${workspaceFolder}`. In `angelscript.modules`, use `folder` for every script under a directory or `entry` for one script and the files it reaches through `#include`. Module boundaries let the editor check whether an `external shared` declaration has a matching definition in another module.

### Example A: Sven Co-op (Folder Module)

If all scripts under `maps/hcas` build together, give that folder a module name.

```jsonc
{
  "angelscript.include.implicitExtension": true,
  "angelscript.searchDirectories": ["${workspaceFolder}/maps"],
  "angelscript.modules": [
    {
      "name": "HCAS",
      "folder": "${workspaceFolder}/maps/hcas"
    }
  ],
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

### Example B: Sven Co-op (Entry Script Module)

If your game starts from `ins2_register.as`, use that file as the entry point. Its includes join the same module.

```jsonc
{
  "angelscript.include.implicitExtension": true,
  "angelscript.searchDirectories": ["${workspaceFolder}/maps"],
  "angelscript.modules": [
    {
      "name": "MapInit",
      "entry": "${workspaceFolder}/maps/ins2/ins2_register.as"
    }
  ],
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

### Example C: Generic Game Engine

If your engine keeps shared includes and API declarations in separate directories, give each directory or file its own path in the settings.

```jsonc
{
  "angelscript.searchDirectories": ["${workspaceFolder}/scripts/include"],
  "angelscript.modules": [
    {
      "name": "GameCore",
      "folder": "${workspaceFolder}/scripts/game"
    }
  ],
  "angelscript.predefinedFiles": ["${workspaceFolder}/scripts/api/engine.as.predefined"]
}
```

## Predefined Game API Files (`.as.predefined`)

Add `game.as.predefined` beside `main.as` and put `int HostScore();` in it. In `main.as`, use `int main() { return HostScore(); }`. Hover over `HostScore` in the call to see `int HostScore()` from the stub.

### How stubs are loaded

- The workspace scan looks for files with the `angelscript.predefinedExtension` suffix, which defaults to `.as.predefined`.
- Use `angelscript.predefined.active` to choose a discovered stub and `angelscript.predefinedFiles` to load explicit paths alongside it, including paths outside the workspace.
- An empty `angelscript.predefined.active` selects the first discovered stub in path order. Use `"all"` to load every discovered stub.
- Run **Select Predefined Stub** or **Select Predefined Stubs** from the Command Palette. You can also open the actions menu from the **AngelScript IntelliSense** language status item.

For a Sven Co-op 5.26 example, open [predefined/sven.as.predefined](../predefined/sven.as.predefined).

## Engine Rules (`angelscript.rules.json`)

If your engine calls `MapInit` from C++, add it to the engine rules to clear the unused-function warning. You can also use rules to link `BaseClass` to a native class in a stub or flag calls and declarations that break your host’s rules.

The server loads one rules file. It tries an existing file from `angelscript.rulesFile` first, then `angelscript.rules.json` at a workspace root, then files beside the active stub, then beside other known stubs, and finally `*.angelscript.rules.json` at a workspace root. Beside `game.as.predefined`, it tries `game.angelscript.rules.json` before taking the first wildcard match in path order.

Use editor settings to match your host and a rules file to share settings and custom checks. Values declared under `engineProperties` in the loaded file take precedence. For example, `"requireEnumScope": true` there overrides a `false` editor setting. The server combines both `noHandleTypes` lists.

To generate a starter file, run **Configure Host Rules & Predefined API...** in the Command Palette and choose the starter rules option.

For examples of each field, see the [Engine Rules Guide](RULES.md) and [predefined/sven.angelscript.rules.json](../predefined/sven.angelscript.rules.json).

## Engine Options and What Changes in the Editor

Match these settings to the `asEP_*` flags and preprocessor behavior your host uses. The defaults below are the extension’s defaults.

### Language and Syntax Options (`angelscript.engine.*`)

| Setting | Default | What changes in the editor |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Set this to `true` if your host accepts plain `&` for primitive and value parameters such as `int &x`. Otherwise, use a direction such as `&in` or `&out`. |
| `angelscript.engine.privatePropAsProtected` | `false` | With `true`, a derived class can access a base class’s `private` members. |
| `angelscript.engine.disallowGlobalVars` | `false` | With `true`, the editor flags declarations of global variables. |
| `angelscript.engine.propertyAccessorMode` | `2` | With `2`, `get_X()` and `set_X(v)` can expose `obj.X`. With `3`, declare the accessors with `property` to use `obj.X`. |
| `angelscript.engine.allowMultilineStrings` | `false` | With `true`, a regular double-quoted string may span lines. With `false`, use triple quotes (`"""..."""`) for a multiline string. |
| `angelscript.engine.boolConversionMode` | `0` | With `1`, `if (obj)` can use a class’s `opImplConv` or `opConv` to `bool`. With `0`, use an explicit conversion. |
| `angelscript.engine.useCharacterLiterals` | `0` | With `0`, `'A'` is a string and assigning it to `int` raises a type error. With `1`, it is an integer character code. |
| `angelscript.engine.disallowValueAssignForRef` | `false` | With `true`, the editor flags value assignment (`a = b`) on reference types. Use handle assignment (`@a = @b`) where you mean to rebind the handle. |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | `name: value` works in all modes. For `name = value`, `0` reports an error, `1` reports a warning, and `2` accepts it. |
| `angelscript.engine.disableIntegerDivision` | `false` | With `false`, `1 / 2` has an integer type. With `true`, the editor treats division of two integers as a floating-point expression. |
| `angelscript.engine.disallowEmptyListElements` | `false` | With `true`, the editor flags the empty slot in `{1, , 3}`. |
| `angelscript.engine.foreachSupport` | `true` | With `false`, the editor flags `foreach` as unsupported. |
| `angelscript.engine.requireEnumScope` | `false` | With `true`, write `Color::Red`. The editor flags an unqualified `Red`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | With `true`, a class with `Item(int id)` also gets an implicit zero-argument constructor. With `false`, add `Item()` explicitly if you want to write `Item x;`. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | With `true`, the editor accepts non-ASCII letters in identifiers. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | With `true`, identical `shared interface` declarations can appear in multiple files without a duplicate declaration error. |
| `angelscript.engine.compilerWarnings` | `1` | `0` hides compiler warnings, `1` shows them as warnings, and `2` shows them as errors. |
| `angelscript.engine.noHandleTypes` | `[]` | List types that your host registers without handle support, such as `["string", "datetime"]`. The editor reports `as-err-handle-on-no-handle-type` (`E261`) for handles to the listed types. Entries are combined with `noHandleTypes` from `angelscript.rules.json`. |

### Preprocessor and Metadata Options

| Setting | Default | What changes in the editor |
| :--- | :--- | :--- |
| `angelscript.preprocessor.elseSupport` | `false` | With `true`, `#else` can become the active branch when `#if` is false. With `false`, a false `#if` still excludes lines through `#endif`. |
| `angelscript.preprocessor.elifSupport` | `false` | With `true`, the first matching branch in a chain with `#elif` is the one the editor analyzes. |
| `angelscript.preprocessor.ifdefSupport` | `false` | With `true`, `#ifdef` and `#ifndef` select branches using defined words from sources such as `angelscript.define` and stub `#define` lines. |
| `angelscript.preprocessor.defineInScripts` | `false` | With `true`, a `#define WORD` in an active part of a script can affect later `#if` checks. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | For `#pragma`, `"accept"` is silent, `"hint"` shows a hint, and `"error"` reports an error. |
| `angelscript.metadata.enabled` | `false` | With `true`, the editor accepts `[...]` metadata blocks before classes, interfaces, functions, and variables. |

## Formatting and `.clang-format`

Use **Format Document** for the whole script or **Format Selection** for the selected lines, such as one function. Opt-in manual-save and on-type formatting use the same style lookup. **Format Predefined Stub** uses a separate formatter that groups declarations by namespace.

For an existing file, the search starts in its folder and walks up to the filesystem root. In each folder it tries `.clang-format`, `_clang-format`, then `.as-clang-format`. For example, `scripts/_clang-format` wins over a workspace root `.clang-format` for a script in `scripts/`. The first regular file the formatter can open ends the search.

For a missing path or when no style loads, the formatter uses the editor’s indentation and whitespace options together with `angelscript.format.*`. Manual-save formatting starts with four spaces. The default `allman` brace style puts the opening brace of a nonempty function on its own line.

Separate language sections with `---`. The formatter prefers `Language: AngelScript`, then `Language: Cpp`, then the unlabelled section. A language section inherits unlabelled settings above it. Put a preset such as `BasedOnStyle: Google` before your overrides.

| Keys | Example |
| :--- | :--- |
| `IndentWidth`, `UseTab`, `NamespaceIndentation`, `BreakBeforeBraces` | `IndentWidth: 2` with `UseTab: Never` indents with two spaces. |
| `SpaceBeforeParens`, `SpacesInParentheses`, `PointerAlignment`, `ReferenceAlignment` | `SpaceBeforeParens: Always` formats a call as `draw (x)`. |
| `AllowShortBlocksOnASingleLine`, `MaxEmptyLinesToKeep` | `Empty` keeps an existing single-line empty block together. `MaxEmptyLinesToKeep: 1` limits consecutive blank lines in a whole script. |

The formatter ignores `ColumnLimit`, `TabWidth`, `BraceWrapping` subkeys, and options such as `AllowShortFunctionsOnASingleLine`. For example, `ColumnLimit: 80` leaves long lines unwrapped.

Put `// clang-format off` and `// clang-format on` on their own lines around a region. An unmatched `off` runs to the end of the file. Tabs, blank lines, and trailing whitespace inside the region may still change. Set `DisableFormat: true` in the selected style to skip formatting the whole file.

## Everyday Features

Put the switches below in `.vscode/settings.json`. See the [Configuration Guide](CONFIGURATION.md).

- **Rename**. Use **Rename Symbol** on `spawn` to change its declaration and uses in indexed scripts. Host names defined only in a stub, such as `RandomFloat`, are read-only.
  Set `angelscript.features.rename` to `false` to turn it off.
- **References**. Right-click `spawn` and choose **Find All References** to see its uses.
  Set `angelscript.features.references` to `false` to turn it off.
- **Code actions**. Open the lightbulb menu for actions such as **Remove unused variable** on an unused local or **Extract Variable** on an eligible expression.
  Set `angelscript.features.codeAction` to `false` to hide these actions.
- **Signature help**. Type `draw(` to see parameter names and types. In `draw(10,`, the comma can move the highlight to the next parameter.
  Set `angelscript.features.signatureHelp` to `false` to turn it off.
- **Code lens**. Click `2 references` above `spawn` to open its uses when References is enabled. Interfaces and their methods can show implementation counts as labels.
  Set `angelscript.features.codeLens` to `false` to hide the counts. `angelscript.codeLens.showOnEnums` controls counts on members such as `Red` in `enum Color { Red }`.
- **Inlay hints**. See inline parameter names beside arguments. If `draw` declares an `int x` parameter, the call `draw(10)` can show `x:` beside `10`.
  Set `angelscript.features.inlayHints` to `false` to hide the labels.
- **Semantic highlighting**. See names such as parameters and fields colored by their role.
  Set `angelscript.features.semanticTokens` to `false` to keep only the usual grammar-based coloring.
- **Diagnostics**. Read the code next to a message in **Problems** or when you hover over a squiggle. For an unused local, set `"angelscript.diagnosticSeverity": {"W103": "hint"}`. The long code `as-warn-unused-variable` works too.
  To turn off a specific check, set its switch to `false`, for example `angelscript.diagnostics.reportPossibleNullDereference`.

The server sends push diagnostics automatically. With pull diagnostics, the editor requests them. Start the server with `--enable-pull-diagnostics` if your client supports pull.

**Go to Definition** and **Go to Declaration** use the same lookup. **Go to Type Definition** goes to the type, such as `Player` for a variable of that type. A moniker is a project-level identifier the server returns to the editor, such as `Game::spawn`.

## Path Variables

You can use these variables in settings that accept paths, such as `angelscript.searchDirectories`.

| Variable | Expands to |
| :--- | :--- |
| `${workspaceFolder}` | Root folder of the active workspace. |
| `${workspaceFolder:name}` | Root folder of a named workspace in a multi-root setup. |
| `${userHome}` | Current user's home folder. |
| `${env:NAME}` | Value of the environment variable `NAME`. |

## Contributed Commands

Open the Command Palette (`Ctrl+Shift+P` or `Cmd+Shift+P`). The Explorer also has commands for setting up modules and formatting stubs.

| Command | Title | What it does |
| :--- | :--- | :--- |
| `angelscript.configureHostEnvironment` | AngelScript: Configure Host Rules & Predefined API... | Opens the setup menu for stubs and engine rules. |
| `angelscript.ignoreUnusedSymbol` | AngelScript: Ignore Unused Symbol (Mark as Entry Point / Callback) | Adds the selected function or callback to the engine rules so it stops showing as unused. |
| `angelscript.selectPredefined` | AngelScript: Select Predefined Stub | Picks the active stub. |
| `angelscript.selectStubs` | AngelScript: Select Predefined Stubs | Picks several stubs to load together. |
| `angelscript.rescanWorkspace` | AngelScript: Rescan Workspace | Scans the workspace again for scripts and stubs. |
| `angelscript.statusMenu` | AngelScript: AngelScript Status Menu | Opens the menu for module and stub actions. |
| `angelscript.switchActiveModule` | AngelScript: Switch Active Module | Chooses which module is active. |
| `angelscript.showServerLog` | AngelScript: Show Server Log | Shows the language server output in VS Code. |
| `angelscript.openLogsFolder` | AngelScript: Open Logs Folder | Opens the folder that contains the server logs. |
| `angelscript.restartServer` | AngelScript: Restart Server | Restarts the language server. |
| `angelscript.setModuleEntryPoint` | Set as Module Entry Point | Right-click an `.as` file to make it a module entry point. |
| `angelscript.setModuleFolder` | Set as Module Folder | Right-click a folder to assign its scripts to a module. |
| `angelscript.formatPredefinedStub` | Format Predefined Stub | Right-click a `.predefined` file to format its declarations. |
| `angelscript.clearLocalState` | Clear Local Session Overrides | Clears the local module and stub choices and restores the workspace settings. |

## Debugger

Use breakpoints, view the call stack, and inspect variables with the VS Code debugger while scripts run in your host.
The [debugger example project](https://github.com/Gaftherman/angelscript-debugger-example) shows how to set up the host and run a complete debug session.

<p align="center"><img src="https://raw.githubusercontent.com/Gaftherman/angelscript-debugger-example/main/media/debugger.gif" alt="Stepping through AngelScript in VS Code" width="720"></p>
<p align="center"><em>Step through an AngelScript script in VS Code.</em></p>

## Building from Source

You’ll need a C++20 compiler and CMake 3.22 or later for the server. The client uses Node.js and `npm`. The bundle step creates `client/dist/extension.js`, which VS Code loads. Run these commands from the repository root.

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile && npm run bundle
```

See the [Configuration Guide](CONFIGURATION.md) for the full settings catalog and editor examples.

---

[README](../README.md) | [Engine Rules Guide](RULES.md) | [CHANGELOG.md](../CHANGELOG.md)
