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
- **Motor nativo Clang-Format**: Descubrimiento automático de archivos `.clang-format`, `_clang-format` y `.as-clang-format` con soporte completo de opciones de estilo de LLVM Clang-Format (presets `BasedOnStyle` como `LLVM`, `Google`, `Chromium`, `Mozilla`, `WebKit`, `Microsoft`, `GNU`, `Allman`, secciones multilenguaje `Language: AngelScript` / `Cpp`, `BraceWrapping`, `SpaceBeforeParens`, `PointerAlignment`, `ShortBlocks/Functions/If/Loops`, `ReflowComments` y `// clang-format off/on`).
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

<!-- SETTINGS_CATALOG_START -->
#### 1. Configuración general y del servidor

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.server.executablePath` | `""` | Ruta al archivo ejecutable del servidor de lenguaje AngelScript. |
| `angelscript.server.logLevel` | `"debug"` | Especifica el nivel de detalle para los registros de salida del servidor de lenguaje. |
| `angelscript.statusBar.alignment` | `"left"` | De qué lado de la barra de estado se coloca el elemento de AngelScript. Es la vía para llegar al registro del servidor, al reinicio y al selector de stub, así que la izquierda es el valor por defecto: ahí están los contadores de problemas, y ahí es donde ya está la vista cuando un diagnóstico te manda a mirar. |
| `angelscript.statusBar.enabled` | `true` | Indica si se debe mostrar el elemento de AngelScript en la barra de estado del editor. |
| `angelscript.statusBar.showStub` | `false` | Indica si se debe mostrar el nombre del stub predefinido activo en el elemento de la barra de estado en lugar de solo el nombre del lenguaje. |
| `angelscript.dimInactiveRegions` | `true` | Atenúa el código dentro de un bloque `#if` que el preprocesador descarta, igual que la extensión de C++ atenúa sus regiones inactivas. Es una decoración, no un color: atenúa lo que haya producido el resaltado de sintaxis, corchetes incluidos, algo que un token semántico no puede hacer - el editor pinta `(`, `{` y `[` con su propia función de emparejado de corchetes. |
| `angelscript.inactiveRegionOpacity` | `0.55` | Cuánto se sigue viendo el código atenuado dentro de un bloque `#if` descartado. 1 es sin atenuar. Va con `#angelscript.dimInactiveRegions#`, que tiene que estar activo para que esto haga algo. |

#### 2. Configuración del espacio de trabajo y módulos

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.modules` | `[]` | Los módulos de script que construye este espacio de trabajo, como entradas `{ "name": "...", "entry": "..." }`. Un módulo es la unidad de compilación propia de AngelScript, y `entry` es el `.as` a partir del cual lo construye el host: todo lo que ese archivo alcanza mediante `#include` le pertenece.  Vacío por defecto, y vacío no cambia nada. Existe porque `external shared class Foo;` solo compila cuando *otro* módulo declara `shared class Foo` - medido, y con la definición en el mismo módulo el compilador lo rechaza igual. Sin conocer los módulos el servidor no puede distinguir una declaración correcta de una rota, así que se calla; conociéndolos, sí puede.  `entry` acepta las mismas variables `${...}` que el resto de ajustes de ruta. Una entrada puede nombrar un `folder` en lugar de, o además de, un `entry` - un módulo de carpeta es dueño de cada script bajo ella, y la carpeta más profunda gana cuando se anidan. |
| `angelscript.moduleEntryPoint` | `""` | Ruta del script punto de entrada del módulo global. Restringe el cierre de compilación a su grafo acíclico dirigido (DAG) de dependencias hacia adelante. |
| `angelscript.searchDirectories` | `[]` | Rutas de búsqueda de directorios personalizadas para resolver archivos de script incluidos (`#include "path.as"`).  Acepta las variables `${...}` que usa `launch.json`: `${workspaceFolder}`, `${workspaceFolder:nombre}`, `${userHome}` y `${env:NOMBRE}`. VS Code no las expande en los ajustes normales, así que lo hace la extensión - una variable que esta ventana no puede responder se deja en la ruta tal como está escrita y se anota en el registro del servidor. |
| `angelscript.predefined.active` | `""` | Selecciona el único stub predefinido cargado por el barrido del espacio de trabajo.  Cuando está vacío (por defecto), el barrido carga el primer stub que encuentra en orden de ruta e ignora el resto, e indica cuál eligió. Establézcalo en `all` para cargar todos los stubs descubiertos juntos, que es lo que un espacio de trabajo con dos stubs solía hacer por defecto - las declaraciones compartidas se resuelven entonces más de una vez.  No afecta a `#angelscript.predefinedFiles`, que continúa cargándose siempre.  El comando "AngelScript: Seleccionar stub predefinido" completa esta configuración por usted.  Acepta las variables `${...}` que usa `launch.json`: `${workspaceFolder}`, `${workspaceFolder:nombre}`, `${userHome}` y `${env:NOMBRE}`. VS Code no las expande en los ajustes normales, así que lo hace la extensión - una variable que esta ventana no puede responder se deja en la ruta tal como está escrita y se anota en el registro del servidor. |
| `angelscript.predefinedFiles` | `[]` | Archivos de stubs predefinidos que declaran la API de la aplicación host, cargados por ruta. Las rutas absolutas se usan tal cual, por lo que el stub puede residir fuera del espacio de trabajo (p. ej. `C:/Games/svencoop/as.predefined`); las rutas relativas se resuelven con respecto a cada carpeta del espacio de trabajo. Los archivos dentro del espacio de trabajo cuyo nombre termina con `#angelscript.predefinedExtension` se detectan automáticamente y no necesitan una entrada aquí.  Acepta las variables `${...}` que usa `launch.json`: `${workspaceFolder}`, `${workspaceFolder:nombre}`, `${userHome}` y `${env:NOMBRE}`. VS Code no las expande en los ajustes normales, así que lo hace la extensión - una variable que esta ventana no puede responder se deja en la ruta tal como está escrita y se anota en el registro del servidor. |
| `angelscript.stubs.activeFiles` | `[]` | Lista de archivos de stubs predefinidos activos cargados en el índice del espacio de trabajo. Administrado dinámicamente mediante el comando AngelScript: Seleccionar stubs predefinidos. |
| `angelscript.include.implicitExtension` | `false` | Hace que `#include "helper"` encuentre `helper.as`. Desactivado por defecto, que es el comportamiento del propio AngelScript: CScriptBuilder abre exactamente el texto entre comillas y no añade nada, así que `#include "helper"` busca un archivo llamado literalmente `helper`. Actívalo para un host que resuelve el nombre por su cuenta y exige que se omita la extensión - Sven Co-op funciona así, y ahí la forma corta es la grafía correcta. El sufijo que se añade es `angelscript.fileExtension`. |
| `angelscript.forceIncludeFiles` | `[]` | Archivos a incluir forzosamente antes de analizar cualquier módulo o script. |
| `angelscript.exclude` | `["**/.git/**", "**/build/**", "**/node_modules/**"]` | Patrones glob de directorios en los que no descienden los barridos del espacio de trabajo. Admite `?`, `*` dentro de un segmento de ruta y `**` entre segmentos. Definir esto reemplaza los valores predeterminados en lugar de añadirse a ellos. |
| `angelscript.fileExtension` | `".as"` | Sufijo de nombre de archivo de los archivos fuente de AngelScript, utilizado al realizar el barrido del espacio de trabajo para construir el grafo de `#include`. |
| `angelscript.predefinedExtension` | `".as.predefined"` | Sufijo de nombre de archivo que marca un archivo encontrado en el espacio de trabajo como un stub predefinido. Esto es un sufijo, no una ruta - use `#angelscript.predefinedFiles` para cargar un archivo específico. |
| `angelscript.define` | `[]` | Palabras que `#if` considera definidas, coincidentes con las palabras que la aplicación host pasa a `CScriptBuilder::DefineWord`. Un `#if WORD` cuya palabra no esté listada aquí se excluye de la compilación, exactamente como lo hace el preprocesador real, y no se reportan diagnósticos en su interior. Un stub predefinido puede declarar las mismas palabras con `#define WORD`, que usualmente es el mejor lugar para ellas: el stub ya describe la configuración del motor del host y viaja con él. |
| `angelscript.arrayLikeTypes` | `[]` | Nombres de tipos de plantilla cuya lista de inicializadores es una simple repetición de su tipo de elemento, tal como ocurre con `array<T>`. El tipo array predeterminado del motor siempre está incluido.  Esto es una forma abreviada. El mecanismo general es una etiqueta `/// @listpattern {repeat T}` en la clase dentro de su stub `.as.predefined`, copiada del propio registro `asBEHAVE_LIST_FACTORY` del tipo — eso también expresa patrones que esta configuración no puede, como `{repeat {string, ?}}` de `dictionary`. Use esta configuración cuando no pueda editar el stub.  De cualquier manera, tiene que indicarse en lugar de detectarse: `array<T>` y `optional<T>` se declaran de forma idéntica en un stub, y el compilador acepta `array<int> a = {1};` mientras rechaza `optional<int> o = {1};`. |
| `angelscript.enableVirtualMixinDocuments` | `false` | Habilita documentos de texto virtuales experimentales para clases mixin (angelscript-virtual://). Cuando está desactivado, se utiliza la síntesis de símbolos de alto rendimiento. |

#### 3. Configuración de Inlay Hints

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.features.inlayHints` | `true` | Mostrar sugerencias en línea de nombres de parámetros y tipos deducidos. |
| `angelscript.inlayHints.maxParameters` | `0` | Número máximo de sugerencias en línea de parámetros a mostrar para una llamada. 0 significa ilimitado (muestra todos los parámetros). |
| `angelscript.inlayHints.maxLength` | `0` | Longitud máxima de caracteres para las etiquetas de sugerencias en línea de parámetros antes de truncar con '...'. 0 significa ilimitado (nunca truncar). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Suprime las sugerencias de nombres de parámetros cuando el texto del argumento coincide exactamente con el nombre del parámetro. El valor predeterminado es false. |
| `angelscript.inlayHints.enableTooltip` | `true` | Muestra informacion detallada en formato Markdown con el tipo y la firma del parametro al pasar el cursor sobre las sugerencias incrustadas (inlay hints). |
| `angelscript.inlayHints.enableLocation` | `true` | Habilita la navegacion con Ctrl+Clic a la declaracion del parametro desde las sugerencias incrustadas (inlay hints). |
| `angelscript.inlayHints.omittedDefaultArguments` | `"off"` | Muestra pistas de inserción (inlay hints) para argumentos opcionales omitidos con valores por defecto en llamadas a funciones. |

#### 4. Configuración de formato de código

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.features.formatting` | `true` | Habilitar los servicios de formateo (formateo de documentos, rangos, al escribir y al guardar). Al desactivarse, no ocurrirá ningún formateo de código. |
| `angelscript.features.onTypeFormatting` | `false` | Formatear automáticamente el código al escribir caracteres activadores específicos (punto y coma, llave de cierre, salto de línea). |
| `angelscript.format.onSave` | `false` | Formatear todo el documento cuando lo guarde manualmente.  Desactivado por defecto: su editor ya tiene `editor.formatOnSave`, y un servidor de lenguaje que reformatee en cada guardado independientemente sobrescribiría esa elección de forma silenciosa. Los guardados por autoguardado o cambio de foco nunca formatean, independientemente de cómo esté configurado esto - reescribir un archivo mientras todavía está escribiendo en él no es una opción. |
| `angelscript.format.braceStyle` | `"allman"` | Dónde se coloca la llave de apertura de un bloque. Una lista de inicializadores y el cuerpo de una lambda mantienen su llave en la misma línea bajo cualquier estilo. |
| `angelscript.format.spacesInsideParentheses` | `false` | Determina si se deben insertar espacios dentro de los paréntesis (por ejemplo, 'foo( bar )' en lugar de 'foo(bar)'). |
| `angelscript.format.keepEmptyBlocksOnSingleLine` | `true` | Mantener los bloques vacíos en una sola línea (ej. 'ClassName() {}') en lugar de expandirlos en varias líneas. |
| `angelscript.format.pointerAlignment` | `"left"` | Alineación de los calificadores de handle (`@`) y referencia (`&`) en declaraciones y parámetros. Se sobrescribe cuando existe un archivo `.clang-format`, `_clang-format` o `.as-clang-format` en el espacio de trabajo. |

#### 5. Configuración de características LSP y autocompletado

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.features.hover` | `true` | Mostrar información de tipos y documentación al pasar el cursor. |
| `angelscript.hover.stringLiteralLength` | `true` | Muestra la longitud en caracteres de las cadenas literales al pasar el cursor. |
| `angelscript.hover.stringLiteralPathResolution` | `true` | Inspecciona y resuelve cadenas literales que parecen rutas de archivo o recursos respecto al directorio del documento, raices del espacio de trabajo y rutas de busqueda configuradas. |
| `angelscript.hover.assetSearchPaths` | `[]` | Directorios donde buscar archivos de recursos referenciados en cadenas literales cuando la resolucion de rutas esta activada. |
| `angelscript.features.completion` | `true` | Habilitar sugerencias de autocompletado. |
| `angelscript.completion.smartTypeRanking` | `true` | Priorizar sugerencias de autocompletado que coincidan con el tipo de parámetro esperado o el destino de asignación. |
| `angelscript.completion.completeFunctionParens` | `true` | Anadir automaticamente parentesis y posicionar el cursor al autocompletar funciones o metodos. |
| `angelscript.completion.qualifyEnumValues` | `true` | Prefijar automáticamente el nombre del enum al autocompletar valores de enumeración (p. ej. insertando 'EnumName::EnumValue') para evitar ambigüedades. |
| `angelscript.features.definition` | `true` | Habilitar Ir a la definición e Ir a la definición de tipo. |
| `angelscript.features.references` | `true` | Habilitar Buscar todas las referencias. |
| `angelscript.features.signatureHelp` | `true` | Mostrar sugerencias de parámetros al escribir una llamada. |
| `angelscript.features.semanticTokens` | `true` | Habilitar el resaltado semántico de sintaxis. |
| `angelscript.features.documentSymbols` | `true` | Poblar la vista Esquema y las rutas de navegación. |
| `angelscript.features.workspaceSymbols` | `true` | Habilitar la búsqueda de símbolos en todo el espacio de trabajo (Ctrl+T). |
| `angelscript.features.rename` | `true` | Habilitar el cambio de nombre de símbolos. |
| `angelscript.features.documentHighlight` | `true` | Resaltar otras apariciones del símbolo bajo el cursor. |
| `angelscript.features.foldingRange` | `true` | Proporcionar regiones de plegado de código. |
| `angelscript.features.codeAction` | `true` | Ofrecer correcciones rápidas y refactorizaciones. |
| `angelscript.features.documentLink` | `true` | Convertir directivas `#include` en enlaces cliqueables. |
| `angelscript.features.implementation` | `true` | Ir a implementación: desde una interfaz o clase base hacia los tipos que responden a ella, y desde un método hacia los que lo implementan o sobrescriben. |
| `angelscript.features.selectionRange` | `true` | Expandir selección: ampliar la selección un paso sintáctico a la vez. |
| `angelscript.features.callHierarchy` | `true` | Jerarquía de llamadas: quién llama a esta función y a qué llama esta a su vez. |
| `angelscript.features.typeHierarchy` | `true` | Jerarquía de tipos: las bases que declara una clase o interfaz, y los tipos que la declaran como suya. |
| `angelscript.features.linkedEditing` | `true` | Edición vinculada: reescribir una variable local o un parámetro y sus usos juntos, en vivo. Se ofrece solo para nombres que un ámbito léxico mantiene dentro de un solo archivo - cualquier elemento en el ámbito de archivo pasa por Cambiar nombre en su lugar, que busca a través de documentos. |
| `angelscript.features.codeLens` | `true` | Mostrar lentes de código interactivos (referencias, implementaciones) en línea sobre las declaraciones. |
| `angelscript.features.pullDiagnostics` | `true` | Responder a `textDocument/diagnostic` y `workspace/diagnostic` (LSP 3.17). El editor solicita diagnósticos en lugar de esperar a que se le notifiquen. Las notificaciones push se envían de cualquier forma, por lo que desactivar esto no hace perder nada que un cliente que no solicite diagnósticos estuviera utilizando. |
| `angelscript.features.typeConversionChecks` | `true` | Reportar conversiones que no cuenten con un constructor, `opConv`/`opImplConv` o `opCast`/`opImplCast` que las respalde. |
| `angelscript.features.predefinedLoader` | `true` | Cargar archivos de stubs predefinidos que describen la API de la aplicación host. |
| `angelscript.features.enableCommentSuppressions` | `true` | Habilitar la supresión de diagnósticos mediante comentarios usando '// disable <CÓDIGO>' y '// enable <CÓDIGO>' (ej. '// disable W156'). |

#### 6. Configuración de diagnósticos semánticos y análisis

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.diagnosticSeverity` | `{}` | Sobrescribe la gravedad de diagnósticos individuales, indexados por código de diagnóstico. Ejemplo: `{ "as-warn-unused-variable": "hint" }`. |
| `angelscript.diagnostics.reportUnknownTypes` | `true` | Reportar un tipo de parámetro o de retorno que no resuelva a ninguna declaración.  `void f(TypoTypeName x)` es un error de compilación — el motor responde "Identifier 'TypoTypeName' is not a data type in namespace 'TEST' or parent" — y si se deja sin reportar, se manifiesta en cambio como silencio en cada punto de llamada, porque una llamada cuyos tipos de parámetros son desconocidos tampoco se puede verificar.  También es, desde el lado del servidor, indistinguible de un tipo legítimo registrado por el motor. Desactívelo si su host registra tipos en C++ y no declara ninguno de ellos — o mejor aún, configure un archivo stub predefinido mediante `#angelscript.predefinedFiles`. |
| `angelscript.diagnostics.reportAccessorPortability` | `true` | Mostrar sugerencia sobre un accesor `get_`/`set_` escrito sin la palabra clave `property`. Dicho accesor es una propiedad bajo `asEP_PROPERTY_ACCESSOR_MODE` 2, el valor predeterminado de este servidor, pero no bajo el modo 3, que es el del propio motor. Agregar la palabra clave se acepta en ambos, por lo que la corrección rápida no puede romper una compilación funcional - razón por la cual esta opción está activada por defecto. |
| `angelscript.diagnostics.reportAccessorDisabled` | `true` | Mostrar sugerencia donde un accesor de propiedad de script se usa como una propiedad pero el host los deshabilitó (`angelscript.engine.propertyAccessorMode` 0 o 1).  Bajo cualquiera de los dos modos, el compilador omite por completo los accesores definidos por script, por lo que `c.X` respaldado por un `get_X`/`set_X` de script es rechazado — con la palabra clave `property` y sin ella. Es una sugerencia en lugar de un error, y está desactivada por defecto, porque se le está indicando al analizador lo que hace el host: de lo contrario, un host configurado incorrectamente vería errores en código que compila bien para él. No dice nada bajo los modos 2 y 3. |
| `angelscript.diagnostics.reportBoolConversion` | `true` | Mostrar sugerencia cuando se usa una clase donde se espera un `bool`, tal como `if (h)`. Bajo `asEP_BOOL_CONVERSION_MODE` 0, el valor predeterminado del motor, esto es un error de compilación incluso cuando la clase declara `opImplConv`. Una corrección rápida llama al operador de conversión explícitamente, lo cual compila bajo ambos modos. Desactivado por defecto, y completamente silencioso cuando `angelscript.engine.boolConversionMode` es 1. |
| `angelscript.diagnostics.reportMissingFuncdef` | `false` | Mostrar sugerencia cuando una posición de tipo nombra una función en lugar de un tipo, tal como `Foo@ h` donde `Foo` es una función. Un handle de función necesita un `funcdef` para nombrar su firma, y una corrección rápida declara uno a partir de los propios parámetros y tipo de retorno de la función. Desactivado por defecto, ya que el nombre podría pertenecer a un tipo del host que este analizador no puede ver. |
| `angelscript.diagnostics.reportIntegerDivision` | `false` | Mostrar sugerencia sobre `1 / 2`, que se trunca a 0 bajo el valor predeterminado del motor. Desactivado por defecto porque una base de código que busca división entera escribe exactamente esto. |
| `angelscript.diagnostics.reportPossibleNullDereference` | `true` | Advertir cuando se desreferencia un parámetro de handle, un resultado de cast de handle o un handle no inicializado sin una comprobación previa de nulo (`!is null`, retorno temprano de guarda, etc.). Activado por defecto. |
| `angelscript.diagnostics.reportHandleComparisonEquality` | `1` | Configurar la severidad para comparaciones de igualdad de handle con null (`== null`, `!= null`) en lugar de identidad de handle (`is null`, `!is null`). 0: desactivado, 1: advertencia (por defecto del compilador), 2: error. |
| `angelscript.diagnostics.missingAssetPathSeverity` | `"off"` | Severidad del diagnostico para cadenas literales que hacen referencia a archivos de recursos no encontrados. Desactivado por defecto. |

#### 7. Configuración del dialecto del motor y preprocesador (asEP_*)

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Active esto si el host llama a `SetEngineProperty(asEP_ALLOW_UNSAFE_REFERENCES, true)`.  Con esto desactivado — el valor predeterminado del motor —, `&` en un parámetro significa `&inout` y solo un tipo de objeto que admita handles puede usarlo, por lo que `void f(int &x)` es un error. Con esto activado, los primitivos pueden pasarse por referencia y el diagnóstico no se reporta. |
| `angelscript.engine.privatePropAsProtected` | `false` | Active esto si el host llama a `SetEngineProperty(asEP_PRIVATE_PROP_AS_PROTECTED, true)`.  Un miembro `private` sigue entonces la regla de `protected`, por lo que una clase derivada puede acceder a él y el acceso ya no se reporta. |
| `angelscript.engine.disallowGlobalVars` | `false` | Active esto si el host llama a `SetEngineProperty(asEP_DISALLOW_GLOBAL_VARS, true)`.  Cada declaración de variable global es entonces un error de compilación — el motor responde "Global variables have been disabled by the application" — y se reporta como tal. |
| `angelscript.engine.propertyAccessorMode` | `2` | Cómo `get_X()` / `set_X(v)` se convierten en la propiedad virtual `X`, coincidiendo con `SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE, ...)` del host.  `2` — cualquier accesor cuenta. `3` — solo uno que lleve la palabra clave `property` cuenta, y `c.X` sin ella es un error que el motor reporta como "'X' is not a member of 'C'".  El valor predeterminado del propio motor es `3`; el de este servidor es `2`, porque ser indulgente aquí pasa por alto un error, mientras que ser estricto inventa uno para cada host que define `2`. Establézcalo en `3` si el suyo no lo hace. |
| `angelscript.engine.allowMultilineStrings` | `false` | Indica si una cadena simple `"..."` puede abarcar varias líneas, coincidiendo con `SetEngineProperty(asEP_ALLOW_MULTILINE_STRINGS, ...)` del host.  Desactivado en el motor y desactivado aquí: dicha cadena es rechazada con "Multiline strings are not allowed in this application". Un `"""heredoc"""` abarca varias líneas bajo cualquiera de las dos configuraciones. Active esto solo si su host lo hace, o el servidor reportará código que su motor acepta. |
| `angelscript.engine.boolConversionMode` | `0` | Cómo se puede usar una clase donde se espera un `bool`, coincidiendo con `SetEngineProperty(asEP_BOOL_CONVERSION_MODE, ...)` del host.  `0` — nunca; `if (h)` en una clase es un error de compilación incluso cuando la clase declara `opImplConv`. `1` — una clase que declara `opImplConv` u `opConv` se puede usar como condición.  `0` es el valor predeterminado del propio motor y de este servidor. Establézcalo en `1` si su host lo define, o la sugerencia adjunta describirá una restricción que usted no tiene. |
| `angelscript.engine.useCharacterLiterals` | `0` | Cómo se lee `'x'`, coincidiendo con asEP_USE_CHARACTER_LITERALS. `0` — una cadena de un solo carácter, el valor predeterminado del motor, por lo que `int c = 'x'` es un error de compilación. `1` — un código de carácter entero, por lo que la misma línea compila. |
| `angelscript.engine.disallowValueAssignForRef` | `false` | asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE. Cuando el host lo define, `a = b` en un tipo de referencia es un error y se requiere `@a = @b`. |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | asEP_ALTER_SYNTAX_NAMED_ARGS. `0` — solo `name: value`, el valor predeterminado del motor, y `name = value` es un error. `1` — `name = value` se acepta con una advertencia. `2` — se acepta silenciosamente. |
| `angelscript.engine.disableIntegerDivision` | `false` | asEP_DISABLE_INTEGER_DIVISION. Cuando el host lo define, `/` en dos enteros produce un float, por lo que `1 / 2` es 0.5 en lugar de 0. |
| `angelscript.engine.disallowEmptyListElements` | `false` | asEP_DISALLOW_EMPTY_LIST_ELEMENTS. Cuando el host lo define, un hueco en una lista de inicializadores como `{1, , 3}` es un error. |
| `angelscript.engine.foreachSupport` | `true` | asEP_FOREACH_SUPPORT. ACTIVADO por el propio valor predeterminado del motor; desactívelo solo si su host deshabilita `foreach`, de lo contrario, cada bucle `foreach` se reporta. |
| `angelscript.engine.requireEnumScope` | `false` | asEP_REQUIRE_ENUM_SCOPE. Cuando se activa, un enumerador sin calificar deja de resolverse y debe calificarse con el nombre de su tipo enum; de lo contrario, se reporta 'No matching symbol'. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT. ACTIVADO por el propio valor predeterminado del motor; si se desactiva, una clase que solo declare constructores no predeterminados ya no se podrá construir por defecto. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | asEP_ALLOW_UNICODE_IDENTIFIERS. Cuando se activa, se aceptan caracteres Unicode no ASCII en identificadores en lugar de generar errores sintácticos. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | asEP_IGNORE_DUPLICATE_SHARED_INTF. Cuando se activa, declarar la misma interfaz compartida más de una vez compila correctamente en lugar de reportar un conflicto de nombres. |
| `angelscript.engine.compilerWarnings` | `1` | asEP_COMPILER_WARNINGS. Controla la gravedad de las advertencias del compilador: 0 las suprime por completo, 1 las emite como advertencias (predeterminado del motor) y 2 las convierte en errores de compilación. |
| `angelscript.preprocessor.elseSupport` | `false` | Active esto si el host aplicó un parche a su copia de `scriptbuilder.cpp` para comprender `#else`.  El complemento incluido con el SDK no lo hace. Medido frente al compilador real: en `#if FOO / a / #else / b / #endif` con `FOO` sin definir, la rama `#else` **no** se convierte en la tomada — se borra junto con el resto del bloque, porque `#else` no es una directiva y la exclusión se extiende hasta `#endif` de todos modos. Con esto desactivado, eso es exactamente lo que este servidor asume. |
| `angelscript.preprocessor.elifSupport` | `false` | Active esto si el host aplicó un parche a su copia de `scriptbuilder.cpp` para comprender `#elif`.  No está presente en el complemento de serie. Con esto activado, la primera rama cuya palabra esté definida es la activa y todas las demás ramas quedan excluidas. |
| `angelscript.preprocessor.ifdefSupport` | `false` | Active esto si el host aplicó un parche a su copia de `scriptbuilder.cpp` para comprender `#ifdef` y `#ifndef`.  No está presente en el complemento de serie, donde cualquiera de los dos se deja en el código fuente y el compilador reporta `Unexpected token`. |
| `angelscript.preprocessor.defineInScripts` | `false` | Active esto si el host aplicó un parche a su copia de `scriptbuilder.cpp` para que `#define WORD` en un script defina una palabra.  No está presente en el complemento de serie, donde `DefineWord` es una llamada de C++ que realiza el host y un `#define` escrito en un script es un error de sintaxis. Para declarar las palabras que el propio host define, use `#angelscript.define` o una línea `#define` en un stub predefinido, que no se ven afectados por esta configuración. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | Qué reportar para un `#pragma`.  El complemento de serie los rechaza todos: al no haber ningún callback de pragma registrado, sustituye la respuesta del callback por un fallo, escribe `Invalid #pragma directive` y hace fallar toda la sección. Sin embargo, el valor predeterminado aquí es `accept`, porque un host que registra un callback es el caso habitual y reportar un error por defecto pondría un subrayado ondulado en un pragma que compila bien. Elija `error` para un host que realmente no haya registrado nada, o `hint` si no está seguro. |
<!-- SETTINGS_CATALOG_END -->
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
