# Angelscript for VS Code

**[English](README.md)** | **[Español](docs/README.es.md)**

Angelscript adds autocomplete, hover documentation, error checks, and code navigation for AngelScript (`.as`) files in VS Code.

![Hover documentation on a method call](client/images/hover.gif)
*Rest the cursor on a function or method call to see its signature and doc comment.*

## Get started

1. Install the extension from the [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman).
2. Open a folder that contains `.as` script files.
3. Optional: add an `.as.predefined` file to your folder if your game provides built-in types or functions.

## What you get

| Feature | What you see | Image |
| :--- | :--- | :--- |
| Hover | Watch the signature and doc comment pop up when the cursor rests on a symbol. | ![Hover documentation](client/images/hover.gif) |
| Autocomplete | Watch matching methods and fields appear as you type after `.` or `::`. | ![Member autocomplete](client/images/completion.gif) |
| Diagnostics | Watch red and yellow squiggles appear on typos and unused variables as you type. | ![Diagnostics while typing](client/images/diagnostics.gif) |
| Signature help | Parameter names and types stay visible while you fill in a function call. | ![Signature help](client/images/signature-help.png) |
| Outline | The Outline panel lists every namespace, class, field, and method in the file. | ![Outline panel](client/images/outline.png) |

## Tell it what your game offers

Your engine adds built-in classes, functions, and globals that are not defined in your `.as` scripts. You can list them in an `.as.predefined` file, which is a file that lists what the game gives to scripts (often called a stub). With that file in `${workspaceFolder}`, autocomplete and hover recognize your game's API without false errors.

![Predefined game API in autocomplete](client/images/predefined.gif)
*Watch the editor recognize game types and methods as soon as the `.as.predefined` file is loaded.*

![Example .as.predefined file](client/images/predefined-file.png)

## Add your own rules

A `*.angelscript.rules.json` file lets your team set engine options and add its own checks. For example, you can warn when a type is stored in a class field. Put the file in `${workspaceFolder}` and the editor loads it by itself.

![Custom engine rules in action](client/images/rules.gif)
*Watch a custom rule flag a disallowed field type right inside the editor.*

![Example .angelscript.rules.json file](client/images/rules-file.png)

## Match your engine

Each game configures AngelScript with its own language rules, such as whether `foreach` loops or multi-line strings are valid. You can match your game's setup with the `angelscript.engine.*` settings in VS Code. Changing an option updates the editor checks right away.

![Engine option changing editor checks](client/images/engine-options.gif)
*Watch an error appear or clear as an engine option changes in settings.*

## Documentation

- [Detailed Guide](docs/guide.md) — workspace examples, predefined files, engine options, commands, and full settings catalog.
- [Engine Rules Guide](docs/rules.md) — how to write `*.angelscript.rules.json` files.
- [CHANGELOG.md](CHANGELOG.md) — release history and changes.

## Building from source

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile
```

Licensed under the [MIT License](LICENSE).
