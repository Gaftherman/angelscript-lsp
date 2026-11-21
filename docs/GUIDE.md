# Angelscript Detailed Guide

[English](GUIDE.md) | [Español](GUIDE.es.md) | [README](../README.md)

Start with a workspace example that matches how your game loads scripts. Then use the settings below to tell VS Code about the game’s API and language rules.

## Workspace Setup Examples

Put the example in `.vscode/settings.json` under `${workspaceFolder}`. In `angelscript.modules`, use `folder` for every script under a directory or `entry` for one script and the files it reaches through `#include`.

Module boundaries let the editor check whether an `external shared` declaration has a matching definition in another module.

### Example A: Sven Co-op (Folder Module)

If all scripts under `maps/hcas` build together, give that folder a module name.

```jsonc
{
  // Allow extensionless includes (for example, #include "helper" finds helper.as)
  "angelscript.include.implicitExtension": true,

  // Extra search folder for #include paths
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],

  // All scripts under maps/hcas belong to module HCAS
  "angelscript.modules": [
    {
      "name": "HCAS",
      "folder": "${workspaceFolder}/maps/hcas"
    }
  ],

  // Active predefined game API file
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

### Example B: Sven Co-op (Entry Script Module)

If your game starts from `ins2_register.as`, use that file as the entry point. Its includes join the same module.

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
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

### Example C: Generic Game Engine

If your engine keeps shared includes and API declarations in separate directories, give each directory or file its own path in the settings.

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

## Predefined Game API Files (`.as.predefined`)

Add an `.as.predefined` stub to describe the API your host registers in C++. The editor can then suggest the host’s classes and functions, show their signatures on hover, and check how you use them.

### How stubs are loaded

- The workspace scan looks for files with the `angelscript.predefinedExtension` suffix, which defaults to `.as.predefined`.
- Set `angelscript.predefined.active` to the stub you want. Leave it empty to use the first one in path order, or set it to `"all"` to load every stub the scan finds.
- List stubs you always want to load in `angelscript.predefinedFiles`. They can live outside `${workspaceFolder}`. Paths can use `${userHome}` and `${env:NAME}`.
- Run **Select Predefined Stub** or **Select Predefined Stubs** from the Command Palette. You can also open the actions menu from the **AngelScript IntelliSense** language status item.

For a Sven Co-op 5.26 example, open [predefined/sven.as.predefined](../predefined/sven.as.predefined).

## Engine Rules (`*.angelscript.rules.json`)

If your engine calls `MapInit` from C++, add it to the engine rules to clear the unused-function warning. You can also use rules to link `BaseClass` to a native class in a stub or flag calls and declarations that break your host’s rules.

- Put `angelscript.rules.json` at the workspace root for automatic discovery. The server also looks for `*.angelscript.rules.json` files beside stubs and at the root.
- Set `angelscript.rulesFile` in `.vscode/settings.json` to choose a specific file.
- To generate a starter file, run **Configure Host Rules & Predefined API...** in the Command Palette and choose the starter rules option.

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

Set brace placement and spacing with `angelscript.format.*`. When you format a file, the formatter looks in its directory and walks up through parent folders for `.clang-format`, `_clang-format`, or `.as-clang-format`. The supported options it finds override the corresponding editor settings.

You can use `BasedOnStyle`, `Language: AngelScript` or `Cpp` sections, `SpaceBeforeParens`, `PointerAlignment`, and `AllowShortBlocksOnASingleLine`. To leave a region alone, surround it with `// clang-format off` and `// clang-format on`.

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

Use `type: "angelscript"` in your VS Code debug configuration to set breakpoints and inspect the call stack and local variables. Choose `dap`, `asdbg`, or `cdebugger` with `angelscript.debug.defaultProtocol`. Set `angelscript.debug.defaultPort` to match your runtime. The default is `9002`.

## Building from Source

You’ll need a C++20 compiler and CMake 3.22 or later for the server. The client build uses Node.js and `npm`. Run these commands from the repository root.

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile
```

See the [Configuration Guide](CONFIGURATION.md) for the full settings catalog and editor examples.

---

[README](../README.md) | [Engine Rules Guide](RULES.md) | [CHANGELOG.md](../CHANGELOG.md)
