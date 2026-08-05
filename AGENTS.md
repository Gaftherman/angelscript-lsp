# Project Overview: AngelScript Language Server (AngelLSP)

AngelLSP is a high-performance C++20 Language Server Protocol (LSP) implementation for the AngelScript programming language (`.as` files), using Tree-Sitter for AST parsing and symbol table analysis.

## Core Architecture
- **Layer 4**: LSP Orchestrator & Server (`server/src/lsp/`, `server/src/main.cpp`)
- **Layer 3**: Feature Handlers (`server/src/features/`) - hover, definition, completion, semantic tokens, signature help.
- **Layer 2**: Analysis & Symbol Management (`server/src/analysis/`) - `SymbolTable`, `SymbolCollector`, `SymbolResolver`.
- **Layer 1**: Core, Document, Parser & Utilities (`server/src/document/`, `server/src/parser/`, `server/src/utils/`).

## Key Directories
- `server/`: C++20 LSP backend server (`CMakeLists.txt`, `src/`, `include/`).
- `client/`: VS Code extension TypeScript client.
- `tree-sitter-angelscript/`: Tree-Sitter grammar and parser for AngelScript.
- `Testing/`: Unit & integration tests using doctest.

## Common Developer Workflows
- **Build C++ Backend Server:**
  ```powershell
  cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Debug
  cmake --build server/build --config Debug
  ```
- **Run Unit & Integration Tests:**
  ```powershell
  cd server/build
  ctest -C Debug --output-on-failure
  ```
- **Build VS Code Extension Client:**
  ```powershell
  cd client
  npm install
  npm run compile
  ```

## Advanced Hybrid Architecture: Kimi K3 (Director) + Gemini `agy` (Ejecutor)

### Flags de Máxima Eficiencia para `agy`:
- `--dangerously-skip-permissions`: Permite a `agy` modificar archivos y ejecutar scripts en modo no interactivo sin bloquearse pidiendo permisos.
- `--mode accept-edits`: Acepta automáticamente los cambios de código realizados por Gemini.
- `--output-format json`: Retorna la respuesta en formato JSON estructurado para que Kimi pueda parsear el resultado con precisión.

### Comando Estándar de Ejecución de `agy`:
```powershell
agy -p "<prompt>" --dangerously-skip-permissions --mode accept-edits
```

---

### Protocolo de Trabajo y Verificación Obligatoria:

1. **Delegación a Gemini (`agy`):**
   Kimi K3 envía la tarea pesada a `agy` para ahorrar tokens en OpenRouter:
   `agy -p "Modifica <archivo> para aplicar <cambio>" --dangerously-skip-permissions`

2. **Verificación de Calidad (Control de Calidad por Kimi K3):**
   Inmediatamente después de que `agy` termine, Kimi K3 DEBE verificar el trabajo realizado usando `git diff` o revisando el archivo:
   `git diff <archivo>`
   - Kimi K3 analiza el `diff` producido por Gemini.
   - Si detecta algún error de lógica o sintaxis en el cambio de Gemini, Kimi K3 aplica el ajuste fino.

3. **Verificación de Compilación y Tests:**
   Kimi K3 ejecuta la compilación y prueba para garantizar el 100% de funcionamiento:
   `cmake --build server/build --config Debug`
   `ctest --test-dir server/build -C Debug --output-on-failure`
