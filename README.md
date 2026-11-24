# Angelscript for VS Code

**[English](README.md)** | **[Español](docs/README.es.md)**

[![Marketplace](https://vsmarketplacebadges.dev/version-short/Gaftherman.angelscript-gaftherman.svg)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![Visual Studio Marketplace Installs](https://vsmarketplacebadges.dev/installs-short/Gaftherman.angelscript-gaftherman.svg)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![XERJ Corpus Hub contributor](https://img.shields.io/badge/XERJ%20Corpus%20Hub-contributor-1f6feb)](https://hub.xerj.org/contributors/Gaftherman)

Angelscript adds support for `.as` files to VS Code.

AngelScript is a scripting language that game engines such as Sven Co-op use to run plugin and mod code, and the extension shows documentation on hover, suggests completions, and flags errors as you edit.

The C++20 language server uses Tree-Sitter to parse scripts and index their symbols.

<p align="center"><img src="client/images/hover.gif" alt="Hover documentation on a method call" width="720"></p>

## Installation

Install from the [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman) or search for **Angelscript** by Gaftherman in Extensions (`Ctrl+Shift+X`). You can also press `Ctrl+P`, paste `ext install Gaftherman.angelscript-gaftherman`, and press Enter.

To install a VSIX, press `Ctrl+Shift+P`, run **Extensions: Install from VSIX...**, and select `angelscript-*.vsix`.

### Try it in 30 seconds

Open a folder in VS Code, create `hello.as`, and paste the code below. No game or engine is needed.

```angelscript
class Calculator {
    /// Doubles a number.
    int twice(int number) { return number * 2; }
}
int main() {
    Calculator calc;
    return calc.twice(21);
}
```

Hover over `twice` in `calc.twice(21)` to see its signature and doc comment. Change the call to `calc.twcie(21)` to see an error underline.

## Features

<div align="center">

| Feature | Status |
| :--- | :--- |
| Completion | ✅ |
| Hover | ✅ |
| Signature help | ✅ |
| Definition, declaration, and type definition | ✅ |
| References | ✅ |
| Rename | ✅ |
| Document and workspace symbols | ✅ |
| Semantic tokens | ✅ |
| Inlay hints | ✅ |
| Code actions | ✅ |
| Document and range formatting | ✅ |
| Push and pull diagnostics | ✅ |

</div>

<table>
<tr><td><strong>Hover</strong><br>See a symbol's signature and documentation.<br><img src="client/images/hover.gif" alt="Hover" width="460"></td>
<td><strong>Autocomplete</strong><br>Type after a dot to see matching members.<br><img src="client/images/autocomplete.gif" alt="Autocomplete" width="460"></td></tr>
<tr><td><strong>Signature help</strong><br>See which parameter you are filling in as you type a call.<br><img src="client/images/signature-help.gif" alt="Signature help" width="460"></td>
<td><strong>Diagnostics</strong><br>See warnings such as unused variables in the editor.<br><img src="client/images/diagnostics.gif" alt="Diagnostics" width="460"></td></tr>
<tr><td><strong>Go to Definition</strong><br>Jump from a name to its definition.<br><img src="client/images/go-to-definition.gif" alt="Go to Definition" width="460"></td>
<td><strong>Find All References</strong><br>List the places where a symbol is used.<br><img src="client/images/find-references.gif" alt="Find All References" width="460"></td></tr>
<tr><td><strong>Rename</strong><br>Change a script symbol's name and update its uses.<br><img src="client/images/rename.gif" alt="Rename" width="460"></td>
<td><strong>Code actions</strong><br>Open the lightbulb to see fixes and refactorings.<br><img src="client/images/code-actions.gif" alt="Code actions" width="460"></td></tr>
<tr><td><strong>Formatting</strong><br>Format the whole script or just the selected lines.<br><img src="client/images/formatting.gif" alt="Formatting" width="460"></td>
<td><strong>Inlay hints</strong><br>See parameter names beside call arguments.<br><img src="client/images/inlay-hints.gif" alt="Inlay hints" width="460"></td></tr>
<tr><td><strong>Code lens</strong><br>See the reference count above a declaration.<br><img src="client/images/code-lens.gif" alt="Code lens" width="460"></td>
<td><strong>Outline</strong><br>Browse the current script's classes, functions, and other symbols.<br><img src="client/images/outline.gif" alt="Outline" width="460"></td></tr>
<tr><td><strong>Semantic highlighting</strong><br>See parameters and fields colored by what they represent.<br><img src="client/images/semantic-highlighting.gif" alt="Semantic highlighting" width="460"></td>
<td><strong>Host API</strong><br>Get completion for the types and functions declared in your stub.<br><img src="client/images/predefined-api.gif" alt="Host API" width="460"></td></tr>
<tr><td><strong>Engine rules</strong><br>See your team's custom checks in the editor.<br><img src="client/images/engine-rules.gif" alt="Engine rules" width="460"></td>
<td><strong>Engine options</strong><br>Match the checks to the language options your host uses.<br><img src="client/images/engine-options.gif" alt="Engine options" width="460"></td></tr>
<tr><td><strong>Engine API hover</strong><br>Hover over an engine function to see its documentation from a <code>.as.predefined</code> stub.<br><img src="client/images/engine-hover.gif" alt="Engine API hover" width="460"></td></tr>
</table>

## Host API stubs (.as.predefined)

In AngelScript, the host application registers its C++ API so your scripts can call its functions and use its types. AngelLSP loads `.as.predefined` stubs that describe those declarations for completion, hover, and diagnostics.

| Host | Stub | Notes |
| :--- | :--- | :--- |
| Sven Co-op | [Sven Co-op - Gaftherman](https://github.com/Gaftherman/angelscript-lsp/blob/main/predefined/sven.as.predefined) | **Recommended**. Actively maintained for Sven Co-op 5.26+ API bindings, with const-correct declarations, ref qualifiers, and math and engine structs. |
| Sven Co-op | [Sven Co-op - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Sven%20Co-op/as.predefined) | **Legacy**. Kept for older projects. Its bindings are outdated for current engine releases. |
| Trackmania Nations Forever | [Trackmania Nations Forever - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Trackmania%20Nations%20Forever/as.predefined) | Compatible. Includes bindings for `CGameCtnApp` and `MwFastBuffer`. |
| OpenSiv3D | [OpenSiv3D - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/OpenSiv3D/as.predefined) | Compatible. Includes bindings for `Vec2`, `ColorF`, and `Circle`. |

In `.vscode/settings.json`, set `"angelscript.predefined.active": "${workspaceFolder}/path/to/stub.as.predefined"` or use `"all"` to load every discovered stub. The default is `""`, which selects the first stub in path order. You can also run **AngelScript: Select Predefined Stub** or open the **AngelScript IntelliSense** status item to choose one.

<p align="center"><img src="client/images/predefined-api.gif" alt="Host API in completion" width="720"></p>

<p align="center"><img src="client/images/predefined-file.png" alt="Example .as.predefined file" width="720"></p>

## Engine rules

Save `angelscript.rules.json` in the project root. Use the [Engine Rules Guide](docs/RULES.md) to configure your project's checks.

<p align="center"><img src="client/images/engine-rules.gif" alt="A custom rule flagging a field type" width="720"></p>

## Engine options

Use `angelscript.engine.*` in VS Code to match your host's language options. For example, set `angelscript.engine.foreachSupport` to `false` if your host disables `foreach`.

<p align="center"><img src="client/images/engine-options.gif" alt="Editor checks for an engine option" width="720"></p>

## Debugger

The VS Code debugger connects to your game or host so you can debug scripts while they run. Start a host with [asdebugger](https://github.com/Gaftherman/asdebugger) listening on `127.0.0.1:9002`. Save this as `.vscode/launch.json` and select it in **Run and Debug**.

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Attach to Game",
      "type": "angelscript",
      "request": "attach",
      "port": 9002,
      "protocol": "dap"
    }
  ]
}
```

See the [debugger example project](https://github.com/Gaftherman/angelscript-debugger-example) for a host setup.

## Documentation

- [Detailed Guide](docs/GUIDE.md) — workspace examples, predefined files, engine options, and commands.
- [Configuration Guide](docs/CONFIGURATION.md) — settings and editor examples.
- [Engine Rules Guide](docs/RULES.md) — custom checks and shared engine settings.
- [CHANGELOG.md](CHANGELOG.md) — release history and changes.

## Building from source

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile && npm run bundle
```

Licensed under the [MIT License](LICENSE).
