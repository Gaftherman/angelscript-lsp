# Angelscript for VS Code

**[English](README.md)** | **[Español](docs/README.es.md)**

[![Marketplace](https://vsmarketplacebadges.dev/version-short/Gaftherman.angelscript-gaftherman.svg)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![Visual Studio Marketplace Installs](https://vsmarketplacebadges.dev/installs-short/Gaftherman.angelscript-gaftherman.svg)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![XERJ Corpus Hub contributor](https://img.shields.io/badge/XERJ%20Corpus%20Hub-contributor-1f6feb)](https://hub.xerj.org/contributors/Gaftherman)

Angelscript adds support for `.as` files to VS Code.

Games and applications that embed AngelScript use it to run plugin and mod code. The extension shows documentation on hover, suggests completions, and flags errors as you edit.

The C++20 language server uses Tree-Sitter to parse scripts and index their symbols.

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/hover.gif" alt="Hover documentation on a method call" width="720"></p>
<p align="center"><em>Hover over a method call to see its documentation.</em></p>

<table align="center">
  <tr>
    <td align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/as-file-icon.png" alt="AngelScript file icon" width="64"></td>
    <td align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/as-predefined-icon.png" alt="AngelScript predefined file icon" width="64"></td>
  </tr>
  <tr>
    <td align="center"><code>.as</code><br>script</td>
    <td align="center"><code>.as.predefined</code><br>host API stub</td>
  </tr>
</table>

## Installation

Install from the [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman) or search for **Angelscript** by Gaftherman in Extensions (`Ctrl+Shift+X`). You can also press `Ctrl+P`, paste `ext install Gaftherman.angelscript-gaftherman`, and press Enter.

To install a VSIX, press `Ctrl+Shift+P`, run **Extensions: Install from VSIX...**, and select `angelscript-*.vsix`.

### Try it in 30 seconds

Open a folder in VS Code, create `hello.as`, and paste the code below. The extension checks your code as you edit, shows documentation on hover, suggests completions, and underlines errors.

```angelscript
class Calculator {
    /// Doubles a number.
    int twice(int number) { return number * 2; }
}
void Run() {
    Calculator calc;
    calc.twice(21);
}
```

Hover over `twice` in `calc.twice(21)` to see its signature and doc comment. Type `calc.` to see member suggestions. Change the original call to `calc.twcie(21)` to see the editor underline the error.

The extension does not run scripts. AngelScript scripts run inside a host application such as a game. See [Debugger](#debugger) to run and debug scripts in a host.

## Features

### Hover

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/hover.gif" alt="Hover" width="720"></p>
<p align="center"><em>See a symbol's signature and documentation.</em></p>

### Autocomplete

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/autocomplete.gif" alt="Autocomplete" width="720"></p>
<p align="center"><em>Type after a dot to see matching members.</em></p>

### Signature help

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/signature-help.gif" alt="Signature help" width="720"></p>
<p align="center"><em>See which parameter you are filling in as you type a call.</em></p>

### Diagnostics

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/diagnostics.gif" alt="Diagnostics" width="720"></p>
<p align="center"><em>See warnings such as unused variables in the editor.</em></p>

### Go to Definition

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/go-to-definition.gif" alt="Go to Definition" width="720"></p>
<p align="center"><em>Jump from a name to its definition.</em></p>

### Find All References

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/find-references.gif" alt="Find All References" width="720"></p>
<p align="center"><em>List the places where a symbol is used.</em></p>

### Rename

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/rename.gif" alt="Rename" width="720"></p>
<p align="center"><em>Change a script symbol's name and update its uses.</em></p>

### Code actions

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/code-actions.gif" alt="Code actions" width="720"></p>
<p align="center"><em>Open the lightbulb to see fixes and refactorings.</em></p>

### Formatting

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/formatting.gif" alt="Formatting" width="720"></p>
<p align="center"><em>Format the whole script or just the selected lines.</em></p>

### Inlay hints

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/inlay-hints.gif" alt="Inlay hints" width="720"></p>
<p align="center"><em>See parameter names beside call arguments.</em></p>

### Code lens

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/code-lens.gif" alt="Code lens" width="720"></p>
<p align="center"><em>See the reference count above a declaration.</em></p>

### Outline

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/outline.gif" alt="Outline" width="720"></p>
<p align="center"><em>Browse the current script's classes, functions, and other symbols.</em></p>

### Semantic highlighting

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/semantic-highlighting.gif" alt="Semantic highlighting" width="720"></p>
<p align="center"><em>See parameters and fields colored by what they represent.</em></p>

### Host API

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/predefined-api.gif" alt="Host API" width="720"></p>
<p align="center"><em>Get completion for the types and functions declared in your stub.</em></p>

### Engine rules

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/engine-rules.gif" alt="Engine rules" width="720"></p>
<p align="center"><em>See your team's custom checks in the editor.</em></p>

### Engine options

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/engine-options.gif" alt="Engine options" width="720"></p>
<p align="center"><em>Match the checks to the language options your host uses.</em></p>

<details>
<summary>Supported LSP features</summary>

| Category | Feature | What it does |
| :--- | :--- | :--- |
| ✏️ Editing | Completion | Suggests symbols and members as you type. |
|  | Hover | Shows signatures and documentation on hover. |
|  | Signature help | Shows signatures and highlights the active parameter. |
|  | Rename | Renames a symbol and updates its uses. |
|  | Inlay hints | Shows inline parameter names and inferred types. |
|  | Code actions | Offers quick fixes and refactorings. |
|  | Document and range formatting | Formats the entire script or selected lines. |
| 🧭 Navigation | Definition, declaration, and type definition | Jumps to declarations, definitions, or type definitions. |
|  | References | Finds where a symbol is used. |
|  | Document and workspace symbols | Lists symbols in a file or workspace. |
| 🔎 Analysis | Semantic tokens | Highlights symbols by their role in code. |
|  | Push and pull diagnostics | Reports issues automatically or on request. |

</details>

## Host API stubs (.as.predefined)

In AngelScript, the host application registers its C++ API so your scripts can call its functions and use its types. AngelLSP loads `.as.predefined` stubs that describe those declarations for completion, hover, and diagnostics.

| Host | Stub | Notes |
| :--- | :--- | :--- |
| Sven Co-op | [Sven Co-op - Gaftherman](https://github.com/angelscript-tools/angelscript-lsp/blob/main/predefined/sven.as.predefined) | **Recommended**. Actively maintained for Sven Co-op 5.26+ API bindings, with const-correct declarations, ref qualifiers, and math and engine structs. |
| Sven Co-op | [Sven Co-op - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Sven%20Co-op/as.predefined) | **Legacy**. Kept for older projects. Its bindings are outdated for current engine releases. |
| Trackmania Nations Forever | [Trackmania Nations Forever - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Trackmania%20Nations%20Forever/as.predefined) | Compatible. Includes bindings for `CGameCtnApp` and `MwFastBuffer`. |
| OpenSiv3D | [OpenSiv3D - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/OpenSiv3D/as.predefined) | Compatible. Includes bindings for `Vec2`, `ColorF`, and `Circle`. |

In `.vscode/settings.json`, set `"angelscript.predefined.active": "${workspaceFolder}/path/to/stub.as.predefined"` or use `"all"` to load every discovered stub. The default is `""`, which selects the first stub in path order. You can also run **AngelScript: Select Predefined Stub** or open the **AngelScript IntelliSense** status item to choose one.

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-lsp/main/client/images/predefined-file.png" alt="Example .as.predefined file" width="720"></p>
<p align="center"><em>A stub declares a class and its methods for the editor.</em></p>

## Engine rules

Save `angelscript.rules.json` in the project root. Use the [Engine Rules Guide](docs/RULES.md) to configure your project's checks. AngelScript **2.39.0**, the latest release, is the supported engine version. If your host embeds another release, declare it with the `engine` section (`{"engine": {"version": "2.36.1"}}`) and the server reports engine properties that release does not have. The [CLI](https://github.com/angelscript-tools/angelscript-cli/blob/main/docs/ENGINE_VERSIONS.md) lists which official releases it builds and tests against.

## Engine options

Use `angelscript.engine.*` in VS Code to match your host's language options. For example, set `angelscript.engine.foreachSupport` to `false` if your host disables `foreach`.

## Debugger

Debugging lives in its own projects, so any host can use it. [angelscript-dap](https://github.com/angelscript-tools/angelscript-dap) is the debugger, the [example project](https://github.com/angelscript-tools/angelscript-debugger-example) shows a complete host and VS Code setup, and the [AngelScript CLI](https://github.com/angelscript-tools/angelscript-cli) runs and debugs scripts from the terminal with `angelscript --dap-port 9002 script.as`.

Add an `attach` or `launch` configuration of type `angelscript` to `launch.json`, set breakpoints in your `.as` files and press F5.

<p align="center"><img src="https://raw.githubusercontent.com/angelscript-tools/angelscript-dap/main/docs/images/debugger-cli.gif" alt="VS Code attached to a script running in Command Prompt" width="720"></p>

## Building from source

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile && npm run bundle
```

## Documentation

- [Detailed Guide](docs/GUIDE.md) — workspace examples, predefined files, engine options, and commands.
- [Configuration Guide](docs/CONFIGURATION.md) — settings and editor examples.
- [Engine Rules Guide](docs/RULES.md) — custom checks and shared engine settings.
- [CHANGELOG.md](CHANGELOG.md) — release history and changes.

## Roadmap

- Close the code audit and pass the quality gates on the architecture refactor branch.
- Keep diagnostics in step with the official AngelScript compiler.
- Support more hosts through `.as.predefined` stubs.

Progress is recorded in [CHANGELOG.md](CHANGELOG.md).

## Support

Bug reports and feature requests are welcome via [GitHub Issues](https://github.com/angelscript-tools/angelscript-lsp/issues). When the server fails, run **AngelScript: Report Issue** (or press **Report Issue** in the error message). It opens the issue form with your versions, the log and a small anonymized sample of your code already filled in, so you can review everything before it is sent.

If you fix a small bug or add an enhancement, feel free to submit a pull request. [CONTRIBUTING.md](CONTRIBUTING.md) explains how to build and check a change. If you create an `.as.predefined` file for your application, contributing it as an example would be greatly appreciated.

## Related

| Project | Use |
| --- | --- |
| [angelscript-cli](https://github.com/angelscript-tools/angelscript-cli) | Run, check and debug scripts from the terminal |
| [angelscript-dap](https://github.com/angelscript-tools/angelscript-dap) | Debugger core |
| [angelscript-debugger-example](https://github.com/angelscript-tools/angelscript-debugger-example) | A host that shows how to use the debugger |
| [angelscript-jit](https://github.com/angelscript-tools/angelscript-jit) | Optional JIT compiler |
| [angelscript-engine](https://github.com/angelscript-tools/angelscript-engine) | Support for older AngelScript releases |

### Similar projects

Other AngelScript tools exist. If this one does not fit your project, we openly recommend trying them:

- [sashi0034/angel-lsp](https://github.com/sashi0034/angel-lsp): another AngelScript language server for VS Code, with an IntelliJ implementation. Its [examples](https://github.com/sashi0034/angel-lsp/tree/main/examples) include host stubs.

## Credits

Angelscript for VS Code is built on the work of these projects. Their licenses are reproduced in [THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md).

| Project | License | Use |
| --- | --- | --- |
| [Relrin/tree-sitter-angelscript](https://github.com/Relrin/tree-sitter-angelscript) | BSD-3-Clause | Base of the AngelScript grammar. This project keeps a [fork](https://github.com/angelscript-tools/tree-sitter-angelscript). |
| [AngelScript](https://github.com/anjo76/angelscript) by Andreas Jönsson | zlib | The language itself. Its compiler is the test oracle for diagnostics. |
| [tree-sitter](https://github.com/tree-sitter/tree-sitter) | MIT | Incremental parsing. |
| [tree-sitter-doxygen](https://github.com/tree-sitter-grammars/tree-sitter-doxygen) | MIT | Documentation comments. |
| [lsp-framework](https://github.com/leon-bckl/lsp-framework) | MIT | Language Server Protocol library. |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON. |
| [spdlog](https://github.com/gabime/spdlog) | MIT | Logging. |
| [unordered_dense](https://github.com/martinus/unordered_dense) | MIT | Hash containers. |
| [doctest](https://github.com/doctest/doctest) | MIT | Test framework. |

## License

This repository is licensed under the [MIT License](LICENSE).
