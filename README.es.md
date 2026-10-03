# Servidor de Lenguaje para AngelScript (AngelLSP)

**[English](README.md)** | **[Español](README.es.md)**

[![Versión en Visual Studio Marketplace](https://img.shields.io/visual-studio-marketplace/v/Gaftherman.angelscript-gaftherman.svg?label=Marketplace&color=blue)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![Descargas en Visual Studio Marketplace](https://img.shields.io/visual-studio-marketplace/i/Gaftherman.angelscript-gaftherman.svg?color=success)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![Licencia: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

AngelLSP es un servidor del Protocolo de Servidor de Lenguaje (LSP) de alto rendimiento y seguro para hilos para el lenguaje de programación [AngelScript](https://www.angelcode.com/angelscript/) (`.as`), desarrollado de forma nativa en C++20 e impulsado por Tree-Sitter para el análisis de árboles sintácticos concretos, análisis de símbolos y resolución semántica.

A diferencia de los enfoques basados en ejecutar scripts dentro de un entorno host embebido o en la concatenación de código fuente, AngelLSP analiza archivos de código, puntos de entrada de módulos y stubs predefinidos del host directamente desde árboles de sintaxis abstracta. Está diseñado específicamente para los ecosistemas reales de AngelScript (como Sven Co-op, motores de juegos y personalizaciones de `CScriptBuilder`), ofreciendo consultas de hover en sub-milisegundos, comprobaciones de nulos sensibles al flujo, inferencia de tipos y navegación semántica en todo el espacio de trabajo.

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
- **Hover y autocompletado inteligentes**: Tooltips de documentación con aislamiento de sobrecargas en puntos de llamada, representación de docstrings Doxygen (`@brief`, `@param`, `@return`), resolución de constructores, contratos de lambdas (`(anonymous function) -> NombreFuncdef`) y sugerencias de miembros conscientes del ámbito (`.`, `::`).
- **Interfaz interactiva y atenuación de regiones inactivas**: Elemento en la barra de estado ("AngelScript IntelliSense") con selector de stub activo, atenuación automática de bloques del preprocesador inactivos (`#if / #else / #endif`) y pistas de parámetros (inlay hints) con navegación `Ctrl+Click`.
- **Documentos virtuales de mixins**: Inspección de documentos sintéticos (`angelscript-virtual://`) que permite visualización en línea y validación de miembros en el ámbito del host.
- **Inspección de rutas de activos**: Las cadenas que coinciden con rutas de archivos o activos se comprueban en el espacio de trabajo y rutas de búsqueda para existencia y métricas en hover.
- **Dialecto del motor e integración con el host**: Resolución de `#include` sin extensión al estilo Sven Co-op, stubs predefinidos del host (`.as.predefined`) y flags configurables del preprocesador (`#if`, `#define`).
- **Motor nativo Clang-Format**: Soporte completo para [Clang-Format Style Options](https://clang.llvm.org/docs/ClangFormatStyleOptions.html) mediante archivos `.clang-format`, `_clang-format` o `.as-clang-format` (incluyendo presets `BasedOnStyle` como `LLVM`, `Google`, `Chromium`, `Mozilla`, `WebKit`, `Microsoft`, `GNU`, `Allman`, secciones multilenguaje `Language: AngelScript` / `Cpp`, `BraceWrapping`, `SpaceBeforeParens`, `PointerAlignment`, `ShortBlocks/Functions/If/Loops`, `ReflowComments` y `// clang-format off/on`).
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
| **Sven Co-op** | [Sven Co-op - Gaftherman](https://github.com/Gaftherman/angelscript-lsp/blob/main/predefined/sven.as.predefined) | **Recomendado (Recommended)** — Mantenido activamente y actualizado para Sven Co-op 5.26+, con cualificadores `const`, calificadores de referencia y estructuras completas de matemáticas y motor. Recomendado prioritariamente para Sven Co-op. |
| **Sven Co-op** | [Sven Co-op - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Sven%20Co-op/as.predefined) | **Heredado (Legacy)** — Mantenido por retrocompatibilidad con proyectos existentes; desactualizado respecto a las versiones modernas del juego. |
| **Trackmania Nations Forever** | [Trackmania Nations Forever - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Trackmania%20Nations%20Forever/as.predefined) | Compatible — Vinculaciones del host para scripting en Trackmania Nations Forever (`CGameCtnApp`, `MwFastBuffer`, etc.). |
| **OpenSiv3D** | [OpenSiv3D - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/OpenSiv3D/as.predefined) | Compatible — Vinculaciones del framework C++ de videojuegos OpenSiv3D (`Vec2`, `ColorF`, `Circle`, etc.). |

> [!TIP]
> Para configurar un stub activo, añade `"angelscript.predefined.active": "${workspaceFolder}/ruta/al/stub.as.predefined"` o usa `"all"` para fusionar todos los stubs presentes.

---

## Comandos de la extensión

Todos los comandos pueden ejecutarse desde la Paleta de comandos (`Ctrl+Shift+P` / `Cmd+Shift+P`) o menús contextuales:

| Comando | Título | Descripción |
| :--- | :--- | :--- |
| `angelscript.selectPredefined` | **AngelScript: Select Active Host Stub** | Alterna el stub activo `.as.predefined` mediante una lista QuickPick. |
| `angelscript.selectStubs` | **AngelScript: Select Host Stubs** | Diálogo de selección múltiple para habilitar y fusionar varios stubs. |
| `angelscript.rescanWorkspace` | **AngelScript: Rescan Workspace** | Fuerza un reescaneo y reindexado completo de los archivos del espacio. |
| `angelscript.statusMenu` | **AngelScript: Status Menu** | Muestra el menú de estado del servidor, stub activo y accesos rápidos. |
| `angelscript.showServerLog` | **AngelScript: Show Language Server Log** | Enfoca el canal de salida del servidor de lenguaje en la consola de Salida. |
| `angelscript.openLogsFolder` | **AngelScript: Open Logs Folder** | Abre la carpeta de registros locales del servidor en el explorador. |
| `angelscript.restartServer` | **AngelScript: Restart Server** | Detiene y reinicia el proceso del servidor de lenguaje. |
| `angelscript.setModuleEntryPoint` | **AngelScript: Set as Module Entry Point** | Acción del menú contextual en archivos `.as` para configurar el punto de entrada. |
| `angelscript.setModuleFolder` | **AngelScript: Set as Module Folder** | Acción del menú contextual en carpetas para configurar pertenencia del módulo. |
| `angelscript.formatPredefinedStub` | **AngelScript: Format Predefined Stub Header** | Acción del menú contextual en archivos `.predefined` para formatear encabezados. |
| `angelscript.viewMixinExpansion` | **AngelScript: View Mixin Expansion** | Abre el documento virtual sintetizado (`angelscript-virtual://`). |
| `angelscript.peekMixinInline` | **AngelScript: Peek Mixin Inline** | Abre una vista de inspección en línea para ver la clase mixin expandida. |
| `angelscript.openPhysicalSource` | **AngelScript: Open Physical Source** | Navega desde el documento virtual al archivo físico original. |

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

## Arquitectura de Capas y Matriz de Inclusión

El aislamiento de capas es estrictamente validado por `server/scripts/check-layer-includes.py`:

| Capa | Ruta | Permitido incluir (`#include`) | Estrictamente PROHIBIDO incluir |
| :--- | :--- | :--- | :--- |
| **Capa 1: Core / Config** | `core/`, `config/`, `document/`, `parser/`, `utils/` | Su propia capa, bibliotecas estándar C++ | Capas 2, 3 y 4 |
| **Capa 2: Analysis** | `analysis/` | Capa 1, bibliotecas estándar C++ | Capas 3 y 4 |
| **Capa 3: Features** | `features/<feature>/` | Capas 1 y 2 | Características hermanas, Capa 4 |
| **Capa 4: Server / LSP** | `lsp/`, `main.cpp` | Capas 1, 2 y 3 | Ninguna (capa superior) |

---

## Referencia de configuración

### Variables de ruta

| Variable | Expansión |
| :--- | :--- |
| `${workspaceFolder}` | Directorio raíz de la carpeta de trabajo activa. |
| `${workspaceFolder:name}` | Directorio raíz de la carpeta de trabajo nombrada en un entorno multi-raíz. |
| `${userHome}` | Directorio del usuario actual. |
| `${env:NOMBRE}` | Valor de la variable de entorno `NOMBRE` (ej. `${env:SVENCOOP_DIR}`). |

### Configuraciones clave

#### 1. General y Servidor
| Configuración | Por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.server.executablePath` | `""` | Ruta personalizada al binario `angel_lsp`. Deshabilitada en espacios no confiables. |
| `angelscript.server.logLevel` | `"debug"` | Nivel de detalle: `"error"`, `"warn"`, `"info"`, `"debug"` o `"trace"`. |
| `angelscript.statusBar.enabled` | `true` | Controla si el elemento de AngelScript en la barra de estado es visible. |
| `angelscript.statusBar.alignment` | `"left"` | Alineación del elemento en la barra de estado (`"left"` o `"right"`). |
| `angelscript.dimInactiveRegions` | `true` | Atenúa bloques excluidos por el preprocesador (`#if / #else / #endif`). |
| `angelscript.inactiveRegionOpacity` | `0.55` | Opacidad de regiones inactivas (entre `0.1` y `1.0`). |

#### 2. Espacio de trabajo y Módulos
| Configuración | Por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.modules` | `[]` | Módulos de scripts especificados por `"entry"` o `"folder"`. |
| `angelscript.searchDirectories` | `[]` | Directorios adicionales para resolución de directivas `#include`. |
| `angelscript.include.implicitExtension` | `false` | Resuelve `#include "helper"` a `helper.as` sin requerir extensión. |
| `angelscript.predefined.active` | `""` | Stub activo a cargar. Usa `"all"` para fusionar todos los stubs. |
| `angelscript.predefinedFiles` | `[]` | Lista explícita de archivos de stubs (`.as.predefined`). |
| `angelscript.exclude` | `["**/.git/**", "**/build/**", "**/node_modules/**"]` | Patrones glob excluidos del análisis. |
| `angelscript.enableVirtualMixinDocuments` | `false` | Habilita proveedores de documentos virtuales (`angelscript-virtual://`) para mixins. |

#### 3. Inlay Hints
| Configuración | Por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.features.inlayHints` | `true` | Conmutador principal para pistas en línea. |
| `angelscript.inlayHints.maxParameters` | `0` | Límite de pistas de parámetros por llamada (`0` = sin límite). |
| `angelscript.inlayHints.maxLength` | `0` | Longitud máxima de caracteres en pistas antes de truncar (`0` = sin límite). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Oculta la pista si el argumento coincide con el parámetro. |
| `angelscript.inlayHints.omittedDefaultArguments` | `"off"` | Pistas para argumentos por defecto omitidos: `"nameAndValue"`, `"declaration"`, `"off"`. |

#### 4. Formato de código
| Configuración | Por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.format.braceStyle` | `"allman"` | Estilo de llaves: `"allman"` (en nueva línea) o `"kr"` (en la misma línea). |
| `angelscript.format.spacesInsideParentheses` | `false` | Inserta espacios dentro de paréntesis (ej. `foo( bar )`). |
| `angelscript.format.keepEmptyBlocksOnSingleLine` | `true` | Mantiene bloques vacíos en una sola línea (ej. `{}`). |
| `angelscript.format.pointerAlignment` | `"left"` | Alineación de calificadores de handle (`@`) y referencia (`&`): `"left"` (`Foo@ bar`), `"right"` (`Foo @bar`) o `"middle"` (`Foo @ bar`). |

> **Consejo:** Colocar un archivo `.clang-format`, `_clang-format` o `.as-clang-format` en el espacio de trabajo o directorio padre sobrescribe automáticamente los ajustes del editor con soporte completo de opciones YAML de Clang-Format (`BasedOnStyle`, `BraceWrapping`, `PointerAlignment`, `ColumnLimit`, `IndentCaseLabels`, `SortIncludes`, etc.).

#### 5. Diagnósticos y Dialecto del Motor
| Configuración | Por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.diagnosticSeverity` | `{}` | Anulación de severidad por código de diagnóstico. |
| `angelscript.diagnostics.reportPossibleNullDereference` | `true` | Comprobaciones intraprocedimentales de handles nulos. |
| `angelscript.engine.requireEnumScope` | `false` | Cuando está activo (`asEP_REQUIRE_ENUM_SCOPE`), los enums llevan prefijo `Enum::Miembro`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | Cuando está activo (`asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`), genera constructor por defecto. |
| `angelscript.engine.allowUnsafeReferences` | `false` | Cuando está activo (`asEP_ALLOW_UNSAFE_REFERENCES`), permite referencias inseguras. |
| `angelscript.engine.propertyAccessorMode` | `2` | Modo de accesores: `2` (get/set estándar), `3` (exige prefijo). |
| `angelscript.engine.allowMultilineStrings` | `false` | Permite cadenas multilínea sin escape (`asEP_ALLOW_MULTILINE_STRINGS`). |
| `angelscript.engine.disableIntegerDivision` | `false` | Deshabilita el operador de división entera (`asEP_DISABLE_INTEGER_DIVISION`). |

---

## Agradecimientos y Créditos

- **Logo y Marca de AngelScript**: El icono oficial de AngelScript es una adaptación del sitio web de [AngelScript](https://www.angelcode.com/angelscript/) por Andreas Jönsson.
- **Iconos de archivo (`.as` / `.as.predefined`)**: Obtenidos del magnífico [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (específicamente el icono de ActionScript), tomados prestados temporalmente para pruebas mientras se diseñan los iconos personalizados de AngelScript—muchas gracias a Philipp Kief y los contribuidores del proyecto :P.

---

## Licencia

Este proyecto está licenciado bajo la Licencia MIT. Consulta el archivo [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) para obtener más detalles.
