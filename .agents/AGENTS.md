# Protocolo Estándar de Desarrollo (SOP) - AngelScript LSP

Esta guía es la referencia de oro para el diseño, desarrollo, refactorización y prueba de cualquier funcionalidad dentro de la base de código de AngelScript LSP.

---

## 1. La Regla Inquebrantable de los `#include` (Matriz de Capas)

Para evitar dependencias circulares y errores en cascada, revisa esta tabla antes de escribir cualquier `#include`:

| Capa actual de tu archivo | ✅ Puede hacer `#include` de: | ❌ PROHIBIDO hacer `#include` de: |
| --- | --- | --- |
| **Capa 1: Core / Config**<br>(`config/`, `document/`, `parser/`, `utils/`) | Únicamente librerías estándar de C++ (`<string>`, `<vector>`, etc.) o headers de su propia capa. | Capa 2 (Analysis), Capa 3 (Features), Capa 4 (Server). |
| **Capa 2: Analysis**<br>(`analysis/`) | Capa 1 (Core/Config) y librerías C++. | Capa 3 (Features), Capa 4 (Server). |
| **Capa 3: Features**<br>(`features/hover/`, `features/completion/`, etc.) | Capa 1 (Core) y Capa 2 (Analysis). | **Otras Features** (ej. Hover NO incluye Completion) y Capa 4 (Server). |
| **Capa 4: Server / Listener**<br>(`lsp/`, `main.cpp`) | Capa 1, Capa 2 y Capa 3. | Nada (es la capa superior). |

> **Prueba de fuego:** Si abres `HoverHandler.h` y ves un `#include "../completion/CompletionHandler.h"`, **detente inmediatamente**. Ese cambio romperá la modularidad.

---

## 2. Paso a Paso: Cómo agregar o refactorizar una Feature

Cuando vayas a crear una nueva funcionalidad (por ejemplo, `SignatureHelp`) o a corregir una existente, sigue estrictamente estos **5 pasos**:

### Paso 1: Define el contrato puro (`SignatureHelpHandler.h`)

Crea la firma en la **Capa 3**. Debe ser una función **pura** que acepte únicamente referencias constantes (`const &`).

```cpp
#pragma once

#include "analysis/SymbolTable.h"
#include "document/Document.h"
#include <optional>

namespace lsp::features
{
    struct SignatureHelpRequest
    {
        const Document& document;
        const SymbolTable& symbolTable;
        Position position;
    };

    /**
     * @brief Computes signature help info for a function call at a given position.
     * @param request Immutable context needed to calculate signature help.
     * @return Optional SignatureHelp struct; nullopt if position is invalid.
     */
    std::optional<SignatureHelpResult> GetSignatureHelp(const SignatureHelpRequest& request);
}
```

### Paso 2: Implementa la lógica aislada (`SignatureHelpHandler.cpp`)

Escribe el código sin guardar estado global ni variables estáticas.
* Si el cursor no está sobre una llamada a función válida, retorna `std::nullopt` o un resultado vacío.
* **Nunca** lances excepciones (`throw`). Retorna estado vacío si algo falla.

### Paso 3: Crea el test unitario en memoria (`tests/SignatureHelpTest.cpp`)

Prueba la función **sin tocar el disco duro** usando el helper `TestUtils.h`:

```cpp
#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include "features/signature_help/SignatureHelpHandler.h"

TEST_CASE("ShouldProvideArgumentsForFunctionCall")
{
    // 1. Crear documento ficticio en memoria
    std::string code = "void test(int a, float b) {}\nvoid main() { test(|); }";
    auto doc = angel_lsp::test::CreateTestDocument("file:///test.as", code);
    angel_lsp::analysis::SymbolTable table;
    angel_lsp::test::PopulateTestSymbolTable(doc, table);

    // 2. Ejecutar la función pura...
}
```

### Paso 4: Registra el Kill-Switch y la Capacidad

1. **Verifica `ServerConfig.h`:** Asegúrate de que exista la bandera correspondiente (`enableSignatureHelp`).
2. **Actualiza `Server.cpp`:**
   * En `ComputeCapabilities()` / `Initialize`: Anuncia la capacidad al cliente **solo si** la bandera está activa.
   * En los handlers: Envuelve la llamada con el protector del flag.

```cpp
messageHandler->add<lsp::requests::TextDocument_SignatureHelp>(
    [this](lsp::requests::TextDocument_SignatureHelp::Params &&req)
    {
        if (!m_config.features.enableSignatureHelp)
        {
            return lsp::requests::TextDocument_SignatureHelp::Result{};
        }
        // ...
    });
```

### Paso 5: Compila y Valida

Corre los comandos de verificación en tu terminal:

```powershell
# 1. Compilar todo
cmake --build server/build --config Debug

# 2. Ejecutar los tests con CTest
cd server/build
ctest -C Debug --output-on-failure
```

---

## 3. Estilo de Código y Documentación

1. **Allman Style**: Apertura de llaves `{` siempre en una línea nueva para clases, structs, funciones, loops e `if`/`switch`.
2. **Comentarios Doxygen en Inglés**: Documentar cada clase, función y struct usando formato `/** ... */` en inglés.
