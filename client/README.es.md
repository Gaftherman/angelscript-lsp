# Angelscript - Servidor de Lenguaje para Angelscript (Extensión de VS Code)

**[English](README.md)** | **[Español](README.es.md)**

[![Versión en Visual Studio Marketplace](https://img.shields.io/visual-studio-marketplace/v/Gaftherman.angelscript-gaftherman.svg?label=Marketplace&color=blue)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![Descargas en Visual Studio Marketplace](https://img.shields.io/visual-studio-marketplace/i/Gaftherman.angelscript-gaftherman.svg?color=success)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![Licencia: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Angelscript ofrece soporte de lenguaje avanzado y de alto rendimiento para el lenguaje de programación [AngelScript](https://www.angelcode.com/angelscript/) (`.as`), impulsado por un servidor de lenguaje nativo en C++20 que utiliza Tree-Sitter para el análisis de árboles sintácticos concretos, análisis de símbolos y resolución semántica.

Todo el espacio de trabajo se analiza directamente a partir de árboles sintácticos sin volcados intermedios en disco, concatenación de scripts ni ejecución en el motor host. Está diseñado específicamente para los ecosistemas reales de AngelScript (como Sven Co-op, motores de videojuegos e integraciones personalizadas de `CScriptBuilder`), ofreciendo consultas de hover en sub-milisegundos, comprobaciones de nulos sensibles al flujo, inferencia de tipos y navegación semántica en todo el proyecto.

---

## Guía de inicio rápido

### 1. Instalación

Instala la extensión desde el [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman) o busca `Angelscript` por Gaftherman en la pestaña de Extensiones (`Ctrl+Shift+X`):
1. Abre Visual Studio Code.
2. Presiona `Ctrl+P`, pega `ext install Gaftherman.angelscript-gaftherman` y presiona Enter.

Alternativamente, para instalar desde un paquete `.vsix` compilado:
1. Presiona `Ctrl+Shift+P` (o `Cmd+Shift+P` en macOS) y ejecuta `Extensions: Install from VSIX...`.
2. Selecciona el archivo compilado (`angelscript.vsix` o `angelscript-*.vsix`).

### 2. Configuración del espacio de trabajo

Abre tu carpeta de proyecto en VS Code. Configura `.vscode/settings.json` según la arquitectura de tu motor:

#### Ejemplo A: Sven Co-op (Módulo por carpeta)
Recomendado para paquetes de scripts de mapas en Sven Co-op (ej. `maps/hcas`):
```jsonc
{
  // Permite inclusiones sin extensión (ej. #include "helper" resuelve a helper.as)
  "angelscript.include.implicitExtension": true,

  // Directorios adicionales de búsqueda para rutas #include relativas o entre corchetes angulares
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],

  // Módulo por carpeta (todos los scripts bajo maps/hcas pertenecen al módulo HCAS)
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
Recomendado cuando el árbol de scripts se compila desde un archivo de registro principal:
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

#### Ejemplo C: Motor genérico / Espacio de trabajo independiente
Para integraciones independientes con rutas de inclusión y stubs de API propios:
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
| **Sven Co-op** | [Sven Co-op - Gaftherman](https://github.com/Gaftherman/angelscript-lsp/blob/main/predefined/sven.as.predefined) | **Recomendado (Recommended)** — Mantenido activamente y actualizado para Sven Co-op 5.26+, con cualificadores `const`, calificadores de referencia y estructuras completas de matemáticas y motor. Recomendamos ampliamente este stub para todo desarrollo en Sven Co-op. |
| **Sven Co-op** | [Sven Co-op - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Sven%20Co-op/as.predefined) | **Heredado (Legacy)** — Conservado para retrocompatibilidad con proyectos y configuraciones antiguas; desactualizado frente a las versiones modernas del juego. |
| **Trackmania Nations Forever** | [Trackmania Nations Forever - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Trackmania%20Nations%20Forever/as.predefined) | Compatible — Vinculaciones del host para scripting en Trackmania Nations Forever (`CGameCtnApp`, `MwFastBuffer`, etc.). |
| **OpenSiv3D** | [OpenSiv3D - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/OpenSiv3D/as.predefined) | Compatible — Vinculaciones del framework C++ de videojuegos OpenSiv3D (`Vec2`, `ColorF`, `Circle`, etc.). |

> [!TIP]
> Para configurar un stub activo en tu espacio de trabajo, define `"angelscript.predefined.active": "${workspaceFolder}/ruta/al/stub.as.predefined"` o usa `"all"` para combinar múltiples stubs. También puedes hacer clic en el elemento de la barra de estado o ejecutar `AngelScript: Select Active Host Stub` para cambiar stubs interactivamente.

---

## Comandos de la extensión

Todos los comandos pueden ejecutarse desde la Paleta de comandos (`Ctrl+Shift+P` / `Cmd+Shift+P`) o menús contextuales:

| Comando | Título | Contexto / Descripción |
| :--- | :--- | :--- |
| `angelscript.selectPredefined` | **AngelScript: Select Active Host Stub** | Abre un selector QuickPick para alternar el stub de host `.as.predefined` activo. |
| `angelscript.selectStubs` | **AngelScript: Select Host Stubs** | Abre un cuadro de diálogo de selección múltiple para habilitar y fusionar varios stubs. |
| `angelscript.rescanWorkspace` | **AngelScript: Rescan Workspace** | Fuerza un reescaneo y reindexado completo en segundo plano de todos los archivos del espacio. |
| `angelscript.statusMenu` | **AngelScript: Status Menu** | Muestra el menú de estado del servidor, stub activo y acciones rápidas. |
| `angelscript.showServerLog` | **AngelScript: Show Language Server Log** | Enfoca el canal de salida del servidor de lenguaje en el panel de Salida. |
| `angelscript.openLogsFolder` | **AngelScript: Open Logs Folder** | Abre en el explorador del sistema la carpeta donde se guardan los archivos de registro. |
| `angelscript.restartServer` | **AngelScript: Restart Server** | Detiene y reinicia el proceso del servidor de lenguaje. |
| `angelscript.setModuleEntryPoint` | **AngelScript: Set as Module Entry Point** | Opción del menú contextual en archivos `.as` para definir el punto de entrada del módulo. |
| `angelscript.setModuleFolder` | **AngelScript: Set as Module Folder** | Opción del menú contextual en carpetas para definir la pertenencia de un módulo por directorio. |
| `angelscript.formatPredefinedStub` | **AngelScript: Format Predefined Stub Header** | Opción del menú contextual en archivos `.predefined` para formatear encabezados de API del host. |
| `angelscript.viewMixinExpansion` | **AngelScript: View Mixin Expansion** | Abre el documento virtual sintetizado (`angelscript-virtual://`) para una clase mixin. |
| `angelscript.peekMixinInline` | **AngelScript: Peek Mixin Inline** | Abre una vista de inspección en línea (peek) mostrando la implementación expandida del mixin. |
| `angelscript.openPhysicalSource` | **AngelScript: Open Physical Source** | Navega desde un documento virtual de mixin de vuelta al archivo de código físico original. |

---

## Características principales e interfaz de usuario

- **Barra de estado interactiva**: El elemento "AngelScript IntelliSense" en la barra de estado muestra el estado del servidor y el nombre del stub predefinido activo. Al hacer clic se abre el menú de estado interactivo o el selector de stubs. Alineación configurable con `angelscript.statusBar.alignment` (`"left"` o `"right"`).
- **Atenuación de regiones inactivas del preprocesador**: Los bloques excluidos por directivas `#if / #else / #endif` se atenúan visualmente de forma automática en el editor con opacidad configurable (`angelscript.dimInactiveRegions` y `angelscript.inactiveRegionOpacity`).
- **Inlay Hints con navegación**: Muestra nombres de argumentos en línea. Al presionar `Ctrl+Click` sobre cualquier pista de parámetro se navega directamente a la definición del parámetro formal. Soporta argumentos omitidos por defecto (`nameAndValue`, `declaration`, `off`) y supresión cuando el argumento coincide con el parámetro.
- **Documentos virtuales de mixins (`angelscript-virtual://`)**: Proveedor de documentos virtuales que expande clases mixin dentro del ámbito de la clase destino para inspección en línea, navegación y validación de miembros.
- **Inspección de rutas de activos**: Las cadenas que representan rutas de archivos o activos se comprueban en el espacio de trabajo y rutas de búsqueda (`angelscript.hover.assetSearchPaths`), mostrando existencia en disco, tamaño y métricas al hacer hover.
- **Comprobación de nulos sensible al flujo**: Diagnósticos intraprocedimentales (`as-warn-possible-null-dereference`) que advierten sobre handles no verificados o usados tras asignación a `null`.
- **Hover y navegación precisos**: Tooltips de documentación aislados por sobrecarga, renderizado de docstrings Doxygen (`@brief`, `@param`, `@return`), resolución de constructores, contratos de lambdas, Ir a definición (`F12`) e Ir a implementación (`Ctrl+F12`).
- **Autocompletado inteligente con ranking de tipos**: Clasificación contextual que prioriza tipos coincidentes en argumentos y asignaciones, calificación de valores enum y snippets.
- **Acciones rápidas (Quick Fixes) localizadas**: Correcciones automáticas bilingües en inglés y español para errores de compilador, variables no utilizadas y supresión de diagnósticos.

---

## Internacionalización y Localización (i18n / l10n)

AngelLSP proporciona soporte bilingüe nativo tanto en **inglés** como en **español**:

- **Sincronización automática de idioma**: La extensión se adapta automáticamente al idioma configurado en VS Code (`Configurar idioma de la pantalla` en la Paleta de comandos).
- **Interfaz y configuraciones de la extensión**: Las más de 95 configuraciones, títulos de comandos, elementos de la barra de estado y diálogos están localizados nativamente vía `@vscode/l10n` (`bundle.l10n.json` y `bundle.l10n.es.json`) y tablas de manifiesto NLS (`package.nls.json` y `package.nls.es.json`).
- **Diagnósticos del servidor**: El servidor de lenguaje emite mensajes de diagnóstico en el idioma activo, garantizando que los errores de compilación y las descripciones en hover coincidan con tu idioma preferido.
- **Anulación manual de idioma**: Puedes seleccionar tu idioma explícitamente configurando el argumento de inicio del servidor o pasando `--locale=es` / `--locale=en`.

---

## Referencia de configuración

### Variables de ruta

Las configuraciones de rutas admiten expansión dinámica de variables según el estándar `launch.json` de VS Code:

| Variable | Expansión |
| :--- | :--- |
| `${workspaceFolder}` | Directorio raíz de la carpeta de trabajo activa. |
| `${workspaceFolder:name}` | Directorio raíz de la carpeta de trabajo nombrada en un entorno multi-raíz. |
| `${userHome}` | Directorio del usuario actual. |
| `${env:NOMBRE}` | Valor de la variable de entorno `NOMBRE` (ej. `${env:SVENCOOP_DIR}`). |

---

### Catálogo de configuraciones

#### 1. Configuración general y del servidor

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.server.executablePath` | `""` | Ruta personalizada al binario `angel_lsp`. Deshabilitada en espacios no confiables. |
| `angelscript.server.logLevel` | `"debug"` | Nivel de detalle del log: `"error"`, `"warn"`, `"info"`, `"debug"` o `"trace"`. |
| `angelscript.statusBar.enabled` | `true` | Controla si el elemento de AngelScript en la barra de estado es visible. |
| `angelscript.statusBar.alignment` | `"left"` | Alineación del elemento en la barra de estado (`"left"` o `"right"`). |
| `angelscript.statusBar.showStub` | `false` | Muestra el nombre del stub activo directamente en la etiqueta de la barra de estado. |
| `angelscript.dimInactiveRegions` | `true` | Atenúa visualmente los bloques de código excluidos por el preprocesador (`#if / #else / #endif`). |
| `angelscript.inactiveRegionOpacity` | `0.55` | Opacidad de las regiones inactivas atenuadas (entre `0.1` y `1.0`). |

#### 2. Configuración del espacio de trabajo y módulos

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.modules` | `[]` | Módulos de scripts definidos como `{"name", "entry"}` o `{"name", "folder"}`. |
| `angelscript.moduleEntryPoint` | `""` | Punto de entrada único para el espacio de trabajo si no se usa configuración multi-módulo. |
| `angelscript.searchDirectories` | `[]` | Directorios adicionales para la resolución de `#include "ruta.as"`. |
| `angelscript.include.implicitExtension` | `false` | Permite que `#include "helper"` resuelva a `helper.as` sin requerir la extensión. |
| `angelscript.predefined.active` | `""` | El stub activo a cargar cuando hay varios presentes. Usa `"all"` para fusionar todos. |
| `angelscript.predefinedFiles` | `[]` | Lista explícita de archivos de stubs de API del host (`.as.predefined`). |
| `angelscript.stubs.activeFiles` | `[]` | Lista de rutas de archivos de stubs activos seleccionados en el espacio de trabajo. |
| `angelscript.forceIncludeFiles` | `[]` | Archivos de encabezado incluidos automáticamente en todos los scripts del espacio. |
| `angelscript.exclude` | `["**/.git/**", "**/build/**", "**/node_modules/**"]` | Patrones glob excluidos del escaneo e indexación de archivos. |
| `angelscript.fileExtension` | `".as"` | Extensión que identifica archivos de script de AngelScript en el espacio de trabajo. |
| `angelscript.predefinedExtension` | `".as.predefined"` | Extensión que identifica archivos de stubs de API del host. |
| `angelscript.define` | `[]` | Símbolos globales predefinidos para el preprocesador (ej. `["DEBUG", "CLIENT"]`). |
| `angelscript.arrayLikeTypes` | `[]` | Tipos personalizados tratados como contenedores similares a arrays para inspección. |
| `angelscript.enableVirtualMixinDocuments` | `false` | Habilita proveedores de documentos virtuales (`angelscript-virtual://`) para mixins. |

#### 3. Configuración de Inlay Hints

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.features.inlayHints` | `true` | Conmutador principal para soporte de pistas en línea (inlay hints). |
| `angelscript.inlayHints.maxParameters` | `0` | Límite máximo de pistas de parámetros mostradas por llamada (`0` = sin límite). |
| `angelscript.inlayHints.maxLength` | `0` | Longitud máxima de caracteres en etiquetas de pistas antes de truncar (`0` = sin límite). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Oculta la pista de nombre si el argumento coincide exactamente con el parámetro. |
| `angelscript.inlayHints.enableTooltip` | `true` | Muestra tooltips con documentación detallada al pasar el cursor sobre las pistas. |
| `angelscript.inlayHints.enableLocation` | `true` | Habilita navegación con `Ctrl+Click` a la definición del parámetro desde la pista. |
| `angelscript.inlayHints.omittedDefaultArguments` | `"off"` | Pistas para argumentos por defecto omitidos: `"nameAndValue"`, `"declaration"` u `"off"`. |

#### 4. Configuración de formato de código

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.features.formatting` | `true` | Habilita el formateo de documentos mediante el servidor de lenguaje. |
| `angelscript.features.onTypeFormatting` | `false` | Habilita el formateo mientras se escribe (caracteres de activación: `;`, `}`). |
| `angelscript.format.onSave` | `false` | Formatea automáticamente los documentos de AngelScript al guardar. |
| `angelscript.format.braceStyle` | `"allman"` | Estilo de llaves: `"allman"` (en nueva línea) o `"kr"` (en la misma línea). |
| `angelscript.format.spacesInsideParentheses` | `false` | Inserta espacios dentro de los paréntesis (ej. `foo( bar )` en lugar de `foo(bar)`). |
| `angelscript.format.keepEmptyBlocksOnSingleLine` | `true` | Mantiene bloques vacíos en una sola línea (ej. `{}`) en lugar de expandirlos. |

#### 5. Configuración de características LSP y autocompletado

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.features.hover` | `true` | Habilita tooltips de documentación en hover. |
| `angelscript.hover.stringLiteralLength` | `true` | Muestra la longitud de caracteres y conteo de bytes en hovers sobre literales de cadena. |
| `angelscript.hover.stringLiteralPathResolution` | `true` | Resuelve literales de cadena que parecen rutas de archivos/activos comprobando su existencia. |
| `angelscript.hover.assetSearchPaths` | `[]` | Directorios adicionales para buscar y resolver activos referenciados en cadenas. |
| `angelscript.features.completion` | `true` | Habilita autocompletado inteligente de código. |
| `angelscript.completion.smartTypeRanking` | `true` | Prioriza sugerencias que coinciden con el tipo esperado del parámetro o asignación. |
| `angelscript.completion.completeFunctionParens` | `true` | Inserta automáticamente paréntesis y marcadores de posición al completar funciones. |
| `angelscript.completion.qualifyEnumValues` | `true` | Sugiere valores enum calificados (`Enum::Valor`) cuando el ámbito enum es obligatorio. |
| `angelscript.features.definition` | `true` | Habilita Ir a definición (`F12`). |
| `angelscript.features.references` | `true` | Habilita Buscar todas las referencias (`Shift+F12`). |
| `angelscript.features.signatureHelp` | `true` | Muestra información de firmas mientras se escriben argumentos de función. |
| `angelscript.features.semanticTokens` | `true` | Habilita resaltado sintáctico y semántico avanzado de tokens. |
| `angelscript.features.documentSymbols` | `true` | Habilita símbolos de vista de esquema y migas de pan (breadcrumbs). |
| `angelscript.features.workspaceSymbols` | `true` | Habilita búsqueda difusa de símbolos en todo el proyecto (`Ctrl+T`). |
| `angelscript.features.rename` | `true` | Habilita renombrado de símbolos en todo el espacio de trabajo (`F2`). |
| `angelscript.features.documentHighlight` | `true` | Resalta apariciones del símbolo bajo el cursor. |
| `angelscript.features.foldingRange` | `true` | Habilita plegado de código para bloques, clases, funciones y comentarios. |
| `angelscript.features.codeAction` | `true` | Habilita correcciones rápidas (Quick Fixes). |
| `angelscript.features.documentLink` | `true` | Detecta enlaces navegables en comentarios y rutas de activos. |
| `angelscript.features.implementation` | `true` | Habilita Ir a implementación para interfaces (`Ctrl+F12`). |
| `angelscript.features.selectionRange` | `true` | Habilita expansión inteligente de rangos de selección (`Shift+Alt+Right`). |
| `angelscript.features.callHierarchy` | `true` | Habilita jerarquía bidireccional de llamadas (entrantes y salientes). |
| `angelscript.features.typeHierarchy` | `true` | Habilita navegación de jerarquía de supertipos y subtipos. |
| `angelscript.features.linkedEditing` | `true` | Sincroniza ediciones en identificadores coincidentes. |
| `angelscript.features.codeLens` | `true` | Muestra contadores de referencias y enlaces de accesores sobre declaraciones. |
| `angelscript.features.pullDiagnostics` | `true` | Soporta el modelo pull de diagnósticos de LSP. |
| `angelscript.features.typeConversionChecks` | `true` | Valida conversiones de tipos, casts implícitos y argumentos de constructores. |
| `angelscript.features.predefinedLoader` | `true` | Habilita carga e indexación de stubs de host predefinidos. |
| `angelscript.features.enableCommentSuppressions` | `true` | Reconoce comentarios en línea `// @as-suppress`. |

#### 6. Configuración de diagnósticos semánticos y análisis

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.diagnosticSeverity` | `{}` | Anulación de severidad por código (ej. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.diagnostics.reportUnknownTypes` | `true` | Emite errores cuando se encuentran tipos no resueltos. |
| `angelscript.diagnostics.reportAccessorPortability` | `true` | Advierte sobre patrones de accesores que pueden no ser portables entre motores. |
| `angelscript.diagnostics.reportAccessorDisabled` | `true` | Advierte cuando se usan accesores (`get_`/`set_`) con accesores deshabilitados. |
| `angelscript.diagnostics.reportBoolConversion` | `true` | Advierte sobre conversiones implícitas inseguras a booleanos. |
| `angelscript.diagnostics.reportMissingFuncdef` | `false` | Advierte sobre firmas `funcdef` faltantes o incompatibles. |
| `angelscript.diagnostics.reportIntegerDivision` | `false` | Advierte sobre divisiones enteras que pueden truncar decimales silenciosamente. |
| `angelscript.diagnostics.reportPossibleNullDereference` | `true` | Advierte sobre posibles desreferencias de handles nulos (`as-warn-possible-null-dereference`). |
| `angelscript.diagnostics.reportHandleComparisonEquality` | `1` | Rigor en comparación de handles: `0` (Deshabilitado), `1` (Advertencia), `2` (Error). |
| `angelscript.diagnostics.missingAssetPathSeverity` | `"off"` | Severidad para rutas de activos inexistentes: `"off"`, `"hint"`, `"warning"` o `"error"`. |

#### 7. Configuración del dialecto del motor y preprocesador (`asEP_*`)

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Cuando está activo (`asEP_ALLOW_UNSAFE_REFERENCES`), permite referencias inseguras en firmas. |
| `angelscript.engine.privatePropAsProtected` | `false` | Trata propiedades privadas de clase como protegidas (`asEP_PRIVATE_PROP_AS_PROTECTED`). |
| `angelscript.engine.disallowGlobalVars` | `false` | Prohíbe declaraciones de variables globales (`asEP_DISALLOW_GLOBAL_VARS`). |
| `angelscript.engine.propertyAccessorMode` | `2` | Modo de accesores: `2` (get/set estándar), `3` (exige prefijo de accesor). |
| `angelscript.engine.allowMultilineStrings` | `false` | Permite literales de cadena multilínea sin escape (`asEP_ALLOW_MULTILINE_STRINGS`). |
| `angelscript.engine.boolConversionMode` | `0` | Modo de conversión booleana: `0` (estricto), `1` (permite números/handles). |
| `angelscript.engine.useCharacterLiterals` | `0` | Interpretación de caracteres: `0` (código carácter), `1` (cadena de un carácter). |
| `angelscript.engine.disallowValueAssignForRef` | `false` | Prohíbe asignación de valor para tipos de referencia (`asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE`). |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | Sintaxis de argumentos nombrados: `0` (deshabilitado), `1` (`arg: val`), `2` (`arg = val`). |
| `angelscript.engine.disableIntegerDivision` | `false` | Deshabilita el operador de división entera (`asEP_DISABLE_INTEGER_DIVISION`). |
| `angelscript.engine.disallowEmptyListElements` | `false` | Prohíbe elementos vacíos en listas de inicialización (`asEP_DISALLOW_EMPTY_LIST_ELEMENTS`). |
| `angelscript.engine.foreachSupport` | `true` | Habilita soporte para bucles `foreach`. |
| `angelscript.engine.requireEnumScope` | `false` | Cuando está activo (`asEP_REQUIRE_ENUM_SCOPE`), los enums deben calificarse con `Enum::Miembro`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | Cuando está activo (`asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`), sintetiza constructor por defecto. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | Permite caracteres unicode en identificadores (`asEP_ALLOW_UNICODE_IDENTIFIERS`). |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | Cuando está activo (`asEP_IGNORE_DUPLICATE_SHARED_INTF`), unifica interfaces idénticas. |
| `angelscript.engine.compilerWarnings` | `1` | Severidad de advertencias: `0` (silencioso), `1` (advertencias), `2` (tratar como errores). |
| `angelscript.preprocessor.elseSupport` | `false` | Habilita soporte para la directiva `#else` en scripts. |
| `angelscript.preprocessor.elifSupport` | `false` | Habilita soporte para la directiva `#elif` en scripts. |
| `angelscript.preprocessor.ifdefSupport` | `false` | Habilita soporte para directivas `#ifdef` / `#ifndef` en scripts. |
| `angelscript.preprocessor.defineInScripts` | `false` | Permite a los scripts definir símbolos del preprocesador mediante `#define`. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | Manejo de directivas `#pragma`: `"accept"`, `"hint"` o `"error"`. |

---

## Tipos de archivo y gramáticas TextMate soportadas

AngelLSP registra identificadores de lenguaje dedicados, gramáticas de resaltado sintáctico y colecciones de fragmentos de código para:

- **Scripts de AngelScript (`.as`)**: Identificador de lenguaje `angelscript`. Archivos de script estándar de AngelScript con resaltado sintáctico, reglas de sangría, emparejamiento de llaves y snippets.
- **Stubs de host predefinidos (`.as.predefined`, `.predefined`)**: Identificador de lenguaje `angelscript-predefined`. Definiciones de encabezados de stubs que exponen declaraciones de API de host C++ al servidor de lenguaje.

---

## Confianza y seguridad del espacio de trabajo

AngelLSP implementa límites estrictos de seguridad bajo el modelo de confianza de espacio de trabajo de VS Code:
- En **Espacios de trabajo no confiables**, las rutas de ejecutables de servidor personalizadas configuradas en el espacio de trabajo (`server.executablePath`) están estrictamente deshabilitadas e ignoradas.
- Solo se puede utilizar el ejecutable del servidor integrado o la configuración global del usuario, protegiendo contra la ejecución remota de código mediante configuraciones maliciosas de repositorios.

---

## Agradecimientos y Créditos

- **Logo y Marca de AngelScript**: El icono oficial de AngelScript es una adaptación del sitio web de [AngelScript](https://www.angelcode.com/angelscript/) por Andreas Jönsson.
- **Iconos de archivo (`.as` / `.as.predefined`)**: Obtenidos del magnífico [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (específicamente el icono de ActionScript), tomados prestados temporalmente para pruebas mientras se diseñan los iconos personalizados de AngelScript—todo el crédito y agradecimiento a Philipp Kief y los contribuidores de Material Icon Theme :P.

---

## Licencia

Este proyecto está licenciado bajo la Licencia MIT. Consulta el archivo [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) para obtener más detalles.
