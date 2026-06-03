# angelscript-lsp

**angelscript-lsp** is a Visual Studio Code extension that provides Language Server Protocol (LSP) support for the [AngelScript](https://www.angelcode.com/angelscript/) programming language (`.as` files). 

It features:
- A frontend client written in TypeScript that integrates with VS Code.
- A robust, native C++20 backend server that parses and analyzes AngelScript code.

## Requirements

To build and run this extension from the source, you will need the following dependencies installed on your system:

### For the Client (TypeScript)
- **Node.js**: Recommended v18 or newer.
- **npm**: Comes with Node.js.

### For the Server (C++)
- **CMake**: Version 3.14 or newer.
- **C++20 Compatible Compiler**: 
  - On Windows: Visual Studio 2022 (MSVC) is highly recommended, as the compilation requires MASM for `x64` assembly.
  - On Linux/macOS: GCC 11+ or Clang 14+.

*Note: The C++ backend automatically fetches its dependencies (`nlohmann/json`, `fmt`, and `angelscript`) using CMake's FetchContent during the build process.*

## Installation & Build Instructions

### 1. Build the Backend Server (C++)

Navigate to the `server` directory and build the executable using CMake:

```bash
cd server
mkdir build
cd build
cmake ..
cmake --build . --config Debug
```

*Important for Windows Users:* The VS Code client specifically looks for the generated executable at `../server/build/Debug/angel_lsp.exe` by default. Make sure to build with the `Debug` config (which is default for MSVC) or adjust the extension's entry file.

### 2. Build the Frontend Client (TypeScript)

Next, install the Node dependencies and compile the TypeScript extension code:

```bash
cd ../client
npm install
npm run compile
```

### 3. Running the Extension

1. Open this workspace root directory (`angelscript-lsp`) in VS Code.
2. Press `F5` or go to the Run and Debug container inside VS Code.
3. Select the launch configuration for the extension to launch the `Extension Development Host` window.
4. In the new window, open any folder containing `.as` files to see the LSP in action.

## Extension Settings

(To be documented: Add any `contributes.configuration` settings here once exposed in `package.json`).

## Known Issues

- Currently in early development. Some LSP features like deep semantic renaming or complex refactoring might be missing or experimental. 

## Release Notes

### 0.0.1
- Initial early prototype of the AngelScript C++ Language Server integration.

