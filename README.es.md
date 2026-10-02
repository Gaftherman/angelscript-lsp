# Servidor de Lenguaje para AngelScript (AngelLSP)

**[English](README.md)** | **[Español](README.es.md)**

AngelLSP es un servidor del Protocolo de Servidor de Lenguaje (LSP) de alto rendimiento y seguro para hilos para el lenguaje de programación [AngelScript](https://www.angelcode.com/angelscript/) (`.as`), desarrollado de forma nativa en C++20 e impulsado por Tree-Sitter para el análisis de árboles sintácticos concretos y la resolución semántica.

A diferencia de los enfoques basados en ejecutar scripts dentro de un entorno host embebido o en la concatenación burda del texto de origen, AngelLSP analiza archivos de código, puntos de entrada de módulos y stubs predefinidos del host directamente desde árboles de sintaxis abstracta. Está diseñado específicamente para los ecosistemas reales de AngelScript (como Sven Co-op, motores de juegos y personalizaciones de `CScriptBuilder`), ofreciendo consultas de hover en sub-milisegundos, comprobaciones de nulos sensibles al flujo, inferencia de tipos y navegación semántica en todo el espacio de trabajo.

> [!WARNING]
> ### Estado del Proyecto: En Desarrollo Activo (WIP)
> **AngelLSP se encuentra actualmente bajo desarrollo activo y validación experimental.** Si bien ya proporciona inteligencia de lenguaje enriquecida y verificación continua de paridad frente al compilador de referencia, ciertas construcciones del lenguaje y casos límite continúan evolucionando.
>
> **Alternativa recomendada para producción:**  
> Si necesitas un servidor de lenguaje maduro y probado para trabajo de producción diario o proyectos críticos de AngelScript en este momento, recomendamos enfáticamente utilizar [**sashi0034/angel-lsp**](https://github.com/sashi0034/angel-lsp).

---

## Capacidades Principales

- **Diagnósticos sensibles al flujo**: Comprobaciones intraprocedimentales de desreferencia de punteros y handles nulos (`as-warn-possible-null-dereference`), recuperación de errores sintácticos y validación de paridad frente al compilador oficial.
- **Navegación precisa**: Ir a definición, declaración, definición de tipo e implementación con soporte de sobrecargas (`Ctrl+F12`), y jerarquías bidireccionales de llamadas y tipos.
- **Hover y autocompletado inteligentes**: Tooltips de documentación con aislamiento de sobrecargas en puntos de llamada, representación de docstrings Doxygen (`@brief`, `@param`, `@return`), resolución de contratos de lambdas (`(anonymous function) -> NombreFuncdef`) y sugerencias de miembros conscientes del ámbito (`.`, `::`).
- **Dialecto del motor e integración con el host**: Resolución de `#include` sin extensión al estilo Sven Co-op, stubs predefinidos del host (`.as.predefined`) y flags configurables del preprocesador (`#if`, `#define`).
- **Alto rendimiento y bajo consumo**: C++20 nativo, registro cero en disco por defecto en compilaciones release, flujos de tokens con cero asignaciones dinámicas y seguridad de memoria para el AST.
- **Soporte bilingüe nativo**: Localización dual integrada para diagnósticos, títulos de comandos y ajustes de configuración en inglés (`en`) y español (`es`) mediante `@vscode/l10n`.

---

## Guía de inicio rápido

### 1. Instalación

Instala desde el [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman) o busca `Angelscript` por Gaftherman en la pestaña de Extensiones (`Ctrl+Shift+X`):
1. Abre Visual Studio Code.
2. Presiona `Ctrl+P`, escribe `ext install Gaftherman.angelscript-gaftherman` y presiona Enter.

Alternativamente, para instalar desde un archivo `.vsix`:
1. Presiona `Ctrl+Shift+P` (o `Cmd+Shift+P` en macOS) y selecciona `Extensions: Install from VSIX...`.
2. Selecciona el paquete compilado (`angelscript.vsix` o `angelscript-*.vsix`).

### 2. Configuración del espacio de trabajo

Crea o actualiza el archivo `.vscode/settings.json` en tu carpeta de proyecto de acuerdo a tu motor:

#### Ejemplo A: Sven Co-op (Módulo por carpeta)
Recomendado para paquetes de mapas con múltiples scripts en Sven Co-op (ej. `maps/hcas`):
```jsonc
{
  // Permite inclusiones sin extensión (ej. #include "helper" resuelve a helper.as)
  "angelscript.include.implicitExtension": true,

  // Directorios adicionales de búsqueda para rutas #include relativas y entre corchetes angulares
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],

  // Definición del módulo por carpeta (todos los scripts bajo maps/hcas pertenecen al módulo HCAS)
  "angelscript.modules": [
    {
      "name": "HCAS",
      "folder": "${workspaceFolder}/maps/hcas"
    }
  ],

  // Habilitar inspección de documentos virtuales de mixins (angelscript-virtual://)
  "angelscript.enableVirtualMixinDocuments": true,

  // Stub de host predefinido activo
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

#### Ejemplo B: Sven Co-op (Módulo por script de entrada)
Recomendado cuando el árbol de scripts se compila desde un archivo de registro específico:
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

#### Ejemplo C: Motor genérico / Proyecto independiente
Para integraciones independientes de AngelScript con rutas de inclusión personalizadas y stubs de API:
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

### 3. Stubs de host predefinidos (`.as.predefined`)

En AngelScript, las aplicaciones host registran sus APIs en C++ (clases, funciones globales, propiedades y constantes) dentro del motor en tiempo de ejecución. Para brindar autocompletado e IntelliSense preciso para estas funciones del host, AngelLSP carga encabezados `.as.predefined`.

Están disponibles los siguientes stubs comunitarios probados:

| Entorno del Host | Fuente y Enlace | Estado y Recomendación |
| :--- | :--- | :--- |
| **Sven Co-op** | [Sven Co-op - Gaftherman](https://github.com/Gaftherman/angelscript-lsp/blob/main/predefined/sven.as.predefined) | **Recomendado (Recommended)** — Mantenido activamente y actualizado para Sven Co-op 5.26+, con cualificadores const, calificadores de referencia y estructuras completas de matemáticas/motor. Recomendado prioritariamente para Sven Co-op. |
| **Sven Co-op** | [Sven Co-op - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Sven%20Co-op/as.predefined) | **Heredado (Legacy)** — Mantenido por retrocompatibilidad con proyectos existentes; desactualizado respecto a las versiones modernas del juego. |
| **Trackmania Nations Forever** | [Trackmania Nations Forever - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Trackmania%20Nations%20Forever/as.predefined) | Compatible — Vinculaciones del host para scripting en Trackmania Nations Forever (`CGameCtnApp`, `MwFastBuffer`, etc.). |
| **OpenSiv3D** | [OpenSiv3D - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/OpenSiv3D/as.predefined) | Compatible — Vinculaciones del framework C++ de videojuegos OpenSiv3D (`Vec2`, `ColorF`, `Circle`, etc.). |

> [!TIP]
> Para configurar un stub activo, añade `"angelscript.predefined.active": "${workspaceFolder}/ruta/al/stub.as.predefined"` o usa `"all"` para fusionar todos los stubs presentes.

---

## Compilación desde el código fuente

### Requisitos previos
- Compilador compatible con C++20: MSVC 2022 (v143) en Windows, GCC 13+ o Clang 16+ en Linux/macOS.
- CMake 3.22+ y Ninja (recomendado).
- Node.js 18+ y `npm` (para el cliente de VS Code).

### Comandos de compilación

```bash
# 1. Compilar el servidor de lenguaje C++20
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release

# 2. Ejecutar la suite de pruebas
ctest --test-dir server/build -C Release --output-on-failure

# 3. Compilar el cliente de la extensión de VS Code
cd client && npm install && npm run compile
```

---

## Configuraciones principales

| Configuración | Por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.searchDirectories` | `[]` | Directorios adicionales para la resolución de directivas `#include`. |
| `angelscript.include.implicitExtension` | `false` | Resuelve `#include "helper"` a `helper.as` sin requerir la extensión de archivo. |
| `angelscript.predefinedFiles` | `[]` | Lista de archivos de stubs de API del host (`.as.predefined`). |
| `angelscript.predefined.active` | `""` | El stub activo a cargar. Usa `"all"` para combinar todos los stubs disponibles. |
| `angelscript.modules` | `[]` | Módulos de compilación especificados por archivo de entrada (`"entry"`) o carpeta (`"folder"`). |
| `angelscript.enableVirtualMixinDocuments` | `false` | Habilita proveedores de documentos virtuales (`angelscript-virtual://`) para inspección de mixins. |
| `angelscript.inlayHints.maxParameters` | `0` | Límite máximo de pistas de parámetros por llamada (`0` = sin límite). |
| `angelscript.inlayHints.maxLength` | `0` | Longitud máxima de caracteres en etiquetas de pistas de parámetros (`0` = sin límite). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Oculta la pista de nombre si el argumento coincide exactamente con el parámetro. |
| `angelscript.format.braceStyle` | `"allman"` | Estilo de colocación de llaves (`"allman"` o `"kr"`). |
| `angelscript.format.spacesInsideParentheses` | `false` | Inserta espacios dentro de los paréntesis (ej. `foo( bar )` en lugar de `foo(bar)`). |
| `angelscript.completion.smartTypeRanking` | `true` | Prioriza sugerencias de autocompletado según el tipo esperado en el parámetro o asignación. |
| `angelscript.diagnosticSeverity` | `{}` | Anulación de severidad por código de diagnóstico (ej. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.engine.requireEnumScope` | `false` | Si está activo (`asEP_REQUIRE_ENUM_SCOPE`), los valores enum deben llevar prefijo `Enum::Miembro`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | Si está activo (`asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`), siempre se sintetiza el constructor por defecto. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | Si está activo (`asEP_IGNORE_DUPLICATE_SHARED_INTF`), ignora interfaces compartidas duplicadas. |
| `angelscript.features.*` | `true` | Conmutadores individuales para funcionalidades LSP (hover, completion, formatting, etc.). |

---

## Agradecimientos y Créditos

- **Logo y Marca de AngelScript**: El icono oficial de AngelScript es una adaptación del sitio web de [AngelScript](https://www.angelcode.com/angelscript/) por Andreas Jönsson.
- **Iconos de archivo (`.as` / `.as.predefined`)**: Obtenidos del [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (específicamente el icono de ActionScript), utilizados temporalmente para pruebas mientras se diseñan iconos dedicados de AngelScript—muchas gracias a Philipp Kief y los contribuidores del proyecto :P.

---

## Licencia

Este proyecto está licenciado bajo la Licencia MIT. Consulta el archivo [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) para obtener más detalles.
