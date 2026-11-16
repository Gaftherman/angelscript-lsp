# Guía detallada de Angelscript

**[English](guide.md)** | **[Español](guide.es.md)** | **[README](README.es.md)**

Esta guía explica la configuración del espacio de trabajo, los archivos de API predefinidos, las reglas del motor, las opciones de dialecto, los comandos y todas las configuraciones de la extensión.

## Ejemplos de configuración del espacio de trabajo

Crea o edita `.vscode/settings.json` dentro de `${workspaceFolder}` según cómo organice los scripts tu juego.

Cuando `angelscript.modules` está definido, el editor agrupa los scripts en unidades de compilación llamadas módulos. Así evita que scripts sin relación en la misma carpeta choquen entre sí u oculten errores en tipos `shared`.

### Ejemplo A: Sven Co-op (módulo por carpeta)

Usa esta opción cuando todos los scripts dentro de la carpeta de un mapa pertenecen a un mismo módulo.

```jsonc
{
  // Permite inclusiones sin extensión (por ejemplo, #include "helper" encuentra helper.as)
  "angelscript.include.implicitExtension": true,

  // Carpeta adicional para buscar rutas #include
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],

  // Todos los scripts bajo maps/hcas pertenecen al módulo HCAS
  "angelscript.modules": [
    {
      "name": "HCAS",
      "folder": "${workspaceFolder}/maps/hcas"
    }
  ],

  // Archivo activo con la API predefinida del juego
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

### Ejemplo B: Sven Co-op (módulo por script de entrada)

Usa esta opción cuando tu juego compila los scripts a partir de un único archivo principal de registro.

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
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

### Ejemplo C: Motor genérico o proyecto independiente

Usa esta opción en juegos o motores propios con carpetas de inclusión y archivos de API personalizados.

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

## Archivos de API predefinidos (`.as.predefined`)

Un archivo `.as.predefined` es un archivo que lista lo que el juego ofrece a los scripts (conocido como stub). El juego registra clases, funciones, enumeraciones, variables globales y palabras `#define` desde C++, y los scripts los usan al ejecutarse. Declararlos en un stub permite que el autocompletado, el hover y la revisión de tipos reconozcan la API del juego.

### Cómo se cargan los stubs

- **Búsqueda automática**: Todo archivo terminado en `.as.predefined` dentro de `${workspaceFolder}` se detecta automáticamente.
- **Elegir un stub**: Si hay varios en el proyecto, indica el archivo activo en `angelscript.predefined.active`. Si queda vacío, el editor carga el primero que encuentra.
- **Combinar todos**: Asigna `"all"` a `angelscript.predefined.active` para cargar juntos todos los stubs encontrados.
- **Archivos externos**: Usa `angelscript.predefinedFiles` cuando el archivo esté fuera de `${workspaceFolder}`, o utiliza variables como `${userHome}` y `${env:NOMBRE}`.
- **Selector interactivo**: Haz clic en **AngelScript IntelliSense** en la barra de estado, o ejecuta **Seleccionar stub predefinido** o **Seleccionar stubs predefinidos** desde la Paleta de comandos.

Para Sven Co-op 5.26+, este repositorio incluye un stub mantenido en [predefined/sven.as.predefined](../predefined/sven.as.predefined). Puedes usar el mismo formato para otros juegos como Trackmania Nations Forever u OpenSiv3D.

## Reglas del motor (`*.angelscript.rules.json`)

Muchos juegos llaman funciones como `MapInit` o crean clases de entidad desde C++ sin invocarlas desde otro archivo `.as`. Un archivo `*.angelscript.rules.json` indica al editor cuáles son esos puntos de entrada, conecta propiedades delegadas como `BaseClass` con las clases nativas del stub y añade reglas de seguridad para tu equipo o motor.

- **Carga automática**: Coloca un archivo `*.angelscript.rules.json` en `${workspaceFolder}` o junto a tu archivo `.as.predefined` activo.
- **Ruta explícita**: Define `angelscript.rulesFile` en `.vscode/settings.json` para apuntar a un archivo concreto.
- **Crear plantilla**: Ejecuta **Configurar reglas del motor y API predefinida...** desde la Paleta de comandos.

Para ver ejemplos campo por campo y lo que cambia en el editor, consulta la [Guía de reglas del motor](rules.es.md) y las reglas de referencia para Sven Co-op en [predefined/sven.angelscript.rules.json](../predefined/sven.angelscript.rules.json).

## Opciones del motor y qué cambia en el editor

Cada juego configura AngelScript con sus propias reglas. Los ajustes siguientes reflejan esas banderas del motor (`asEP_*`) y las opciones del preprocesador.

### Opciones de lenguaje y sintaxis (`angelscript.engine.*`)

| Configuración | Por defecto | Qué cambia en el editor |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Con `false`, parámetros como `int &x` muestran un error pidiendo `&in` o `&out`. Con `true`, se acepta `&` sin calificador en tipos primitivos y por valor. |
| `angelscript.engine.privatePropAsProtected` | `false` | Con `false`, leer un miembro `private` de la clase padre desde una clase hija marca un error de acceso. Con `true`, las clases hijas pueden usarlo y aparece en el autocompletado. |
| `angelscript.engine.disallowGlobalVars` | `false` | Con `true`, cualquier variable declarada fuera de una función o clase marca error. Con `false`, las variables globales están permitidas. |
| `angelscript.engine.propertyAccessorMode` | `2` | Con `2`, cualquier método `get_X()` o `set_X(v)` funciona como propiedad `X` en autocompletado y tipos. Con `3`, solo cuentan los métodos con `property` y los demás marcan error de miembro desconocido. |
| `angelscript.engine.allowMultilineStrings` | `false` | Con `false`, los saltos de línea dentro de `"..."` marcan error y exigen usar `"""..."""`. Con `true`, se aceptan cadenas `"..."` en varias líneas. |
| `angelscript.engine.boolConversionMode` | `0` | Con `0`, usar una clase en `if (obj)` marca error aunque defina una conversión a `bool`. Con `1`, las clases con `opImplConv` u `opConv` a `bool` se aceptan en condiciones. |
| `angelscript.engine.useCharacterLiterals` | `0` | Con `0`, `'A'` se toma como `string`, así que asignarlo a `int` marca error de tipo. Con `1`, `'A'` se interpreta como código numérico entero. |
| `angelscript.engine.disallowValueAssignForRef` | `false` | Con `true`, escribir `a = b` en clases de referencia marca error y pide asignación de handle `@a = @b`. Con `false`, `a = b` busca `opAssign`. |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | Con `0`, los argumentos con nombre exigen `nombre: valor` y `nombre = valor` marca error. Con `1`, `nombre = valor` muestra advertencia. Con `2`, ambas formas se aceptan sin avisos. |
| `angelscript.engine.disableIntegerDivision` | `false` | Con `false`, `1 / 2` conserva tipo entero. Con `true`, dividir enteros produce un decimal flotante y asignarlo a `int` activa la comprobación de conversión. |
| `angelscript.engine.disallowEmptyListElements` | `false` | Con `true`, dejar huecos como `{1, , 3}` marca error. Con `false`, se permiten elementos vacíos en listas de inicialización. |
| `angelscript.engine.foreachSupport` | `true` | Con `true`, los bucles `foreach` se analizan y deducen el tipo de la variable. Con `false`, cualquier `foreach` marca un error de función no compatible. |
| `angelscript.engine.requireEnumScope` | `false` | Con `true`, los valores de enum deben escribirse con prefijo como `Color::Red`, los nombres sueltos marcan error y el autocompletado añade el prefijo. Con `false`, se aceptan nombres directos. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | Con `false`, definir un constructor como `Item(int id)` oculta el constructor vacío y `Item x;` marca error salvo que escribas `Item()`. Con `true`, `Item x;` siempre se acepta. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | Con `false`, letras no ASCII en nombres de variables o funciones marcan error de sintaxis. Con `true`, se aceptan identificadores UTF-8. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | Con `false`, repetir una `shared interface` en varios archivos marca error de declaración duplicada. Con `true`, las definiciones idénticas se unifican sin error. |
| `angelscript.engine.compilerWarnings` | `1` | `0` oculta las advertencias del compilador, `1` las muestra como subrayados amarillos y `2` las convierte en errores rojos. |
| `angelscript.engine.noHandleTypes` | `[]` | Nombres de tipos por valor (p. ej. `["string", "datetime"]`) que no admiten handles de objeto (`@`), reportando `as-err-handle-on-no-handle-type` (`E261`). Se combina (unión de conjuntos) con `noHandleTypes` en `angelscript.rules.json`. |

### Opciones de preprocesador y metadatos

| Configuración | Por defecto | Qué cambia en el editor |
| :--- | :--- | :--- |
| `angelscript.preprocessor.elseSupport` | `false` | Con `false`, `#else` se ignora y todo bloque tras un `#if` falso sigue atenuado y omitido hasta `#endif`. Con `true`, la rama `#else` pasa a estar activa cuando `#if` es falso. |
| `angelscript.preprocessor.elifSupport` | `false` | Con `true`, las cadenas `#elif` se evalúan y la primera rama coincidente se analiza en lugar de quedar atenuada. |
| `angelscript.preprocessor.ifdefSupport` | `false` | Con `false`, `#ifdef` y `#ifndef` marcan errores de sintaxis. Con `true`, activan o atenúan bloques según `angelscript.define` y los `#define` del stub. |
| `angelscript.preprocessor.defineInScripts` | `false` | Con `false`, `#define PALABRA` dentro de archivos `.as` marca error de sintaxis. Con `true`, los `#define` en scripts activan los bloques `#if` correspondientes. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | Controla cómo se reporta `#pragma`: `"accept"` no muestra avisos, `"hint"` muestra una sugerencia y `"error"` lo marca como error. |
| `angelscript.metadata.enabled` | `false` | Con `true`, los bloques de metadatos `[...]` antes de clases, interfaces, funciones y variables se aceptan sin error de sintaxis. |

## Formato y `.clang-format`

Usa las opciones `angelscript.format.*` para ajustes rápidos como el estilo de llaves o espacios. Si tu espacio de trabajo o una carpeta superior contiene `.clang-format`, `_clang-format` o `.as-clang-format`, el formateador lo carga automáticamente y usa sus reglas. Incluye soporte para presets `BasedOnStyle`, secciones `Language: AngelScript` o `Cpp`, `BraceWrapping`, `SpaceBeforeParens`, `PointerAlignment`, `ShortBlocks`, `ReflowComments` y comentarios `// clang-format off` / `// clang-format on`.

## Variables de ruta

Las opciones de ruta aceptan estas variables:

| Variable | Significado |
| :--- | :--- |
| `${workspaceFolder}` | Carpeta raíz del proyecto abierto. |
| `${workspaceFolder:name}` | Carpeta raíz de un proyecto concreto en un espacio multi-raíz. |
| `${userHome}` | Carpeta personal del usuario actual. |
| `${env:NOMBRE}` | Valor de la variable de entorno `NOMBRE`. |

## Comandos disponibles

Abre la Paleta de comandos (`Ctrl+Shift+P` o `Cmd+Shift+P`) o haz clic derecho en archivos y carpetas del Explorador:

| Comando | Título | Qué hace |
| :--- | :--- | :--- |
| `angelscript.configureHostEnvironment` | **AngelScript: Configurar reglas del motor y API predefinida...** | Crea una plantilla de reglas para tu proyecto. |
| `angelscript.ignoreUnusedSymbol` | **AngelScript: Ignorar símbolo no usado (marcar como punto de entrada / callback)** | Corrección rápida que añade el símbolo a las exenciones de tus reglas. |
| `angelscript.selectPredefined` | **AngelScript: Seleccionar stub predefinido** | Cambia el archivo `.as.predefined` activo. |
| `angelscript.selectStubs` | **AngelScript: Seleccionar stubs predefinidos** | Selecciona y combina varios archivos `.as.predefined`. |
| `angelscript.rescanWorkspace` | **AngelScript: Volver a examinar el espacio de trabajo** | Reindexa todos los scripts y stubs del proyecto. |
| `angelscript.statusMenu` | **AngelScript: Menu de estado de AngelScript** | Abre el menú con el módulo activo, stub y acciones rápidas. |
| `angelscript.switchActiveModule` | **AngelScript: Cambiar módulo activo** | Cambia el módulo activo analizado en el editor. |
| `angelscript.showServerLog` | **AngelScript: Mostrar registro del servidor** | Muestra el canal de salida del servidor de lenguaje. |
| `angelscript.openLogsFolder` | **AngelScript: Abrir carpeta de registros** | Abre la carpeta donde se guardan los registros del servidor. |
| `angelscript.restartServer` | **AngelScript: Reiniciar servidor** | Reinicia el proceso del servidor de lenguaje. |
| `angelscript.setModuleEntryPoint` | **Definir como punto de entrada del módulo** | Menú contextual en archivos `.as` para fijar el archivo de entrada de un módulo. |
| `angelscript.setModuleFolder` | **Definir como carpeta del módulo** | Menú contextual en carpetas para asignar pertenencia de un módulo. |
| `angelscript.formatPredefinedStub` | **Formatear stub predefinido** | Menú contextual en archivos `.predefined` para dar formato a las declaraciones. |
| `angelscript.clearLocalState` | **Limpiar anulaciones de sesión local** | Borra elecciones guardadas en el estado local del espacio de trabajo. |

## Depurador

La extensión incluye un depurador para VS Code (`type: "angelscript"`) con validación de puntos de interrupción, pila de llamadas e inspección de variables locales. Puedes conectarte por `dap`, `asdbg` o `cdebugger` mediante `angelscript.debug.defaultProtocol` y `angelscript.debug.defaultPort` (puerto `9002` por defecto).

## Compilar desde el código fuente

Requisitos previos:
- Compilador C++20: MSVC 2022 en Windows, GCC 13+ o Clang 16+ en Linux o macOS.
- CMake 3.22+ y Ninja (recomendado).
- Node.js 18+ y `npm` para el cliente de VS Code.

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile
```

## Catálogo completo de configuraciones

<!-- SETTINGS_CATALOG_START -->
#### 1. Configuración general y del servidor

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.server.executablePath` | `""` | Ruta al archivo ejecutable del servidor de lenguaje AngelScript. |
| `angelscript.server.logLevel` | `"debug"` | Especifica el nivel de detalle para los registros de salida del servidor de lenguaje. |
| `angelscript.dimInactiveRegions` | `true` | Atenúa el código dentro de un bloque `#if` que el preprocesador descarta, igual que la extensión de C++ atenúa sus regiones inactivas. Es una decoración, no un color: atenúa lo que haya producido el resaltado de sintaxis, corchetes incluidos, algo que un token semántico no puede hacer - el editor pinta `(`, `{` y `[` con su propia función de emparejado de corchetes. |
| `angelscript.inactiveRegionOpacity` | `0.55` | Cuánto se sigue viendo el código atenuado dentro de un bloque `#if` descartado. 1 es sin atenuar. Va con `#angelscript.dimInactiveRegions#`, que tiene que estar activo para que esto haga algo. |

#### 2. Configuración del espacio de trabajo y módulos

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.configurationTarget` | `"workspace"` | Especifica dónde guardan sus elecciones comandos como 'Establecer punto de entrada del módulo' o 'Seleccionar stub predefinido': en el archivo del espacio de trabajo (.vscode/settings.json), en el estado de sesión local (workspaceState, evitando cambios en Git) o en la configuración global de usuario. |
| `angelscript.modules` | `[]` | Los módulos de script que construye este espacio de trabajo, como entradas `{ "name": "...", "entry": "..." }`. Un módulo es la unidad de compilación propia de AngelScript, y `entry` es el `.as` a partir del cual lo construye el host: todo lo que ese archivo alcanza mediante `#include` le pertenece.  Vacío por defecto, y vacío no cambia nada. Existe porque `external shared class Foo;` solo compila cuando *otro* módulo declara `shared class Foo` - medido, y con la definición en el mismo módulo el compilador lo rechaza igual. Sin conocer los módulos el servidor no puede distinguir una declaración correcta de una rota, así que se calla; conociéndolos, sí puede.  `entry` acepta las mismas variables `${...}` que el resto de ajustes de ruta. Una entrada puede nombrar un `folder` en lugar de, o además de, un `entry` - un módulo de carpeta es dueño de cada script bajo ella, y la carpeta más profunda gana cuando se anidan. |
| `angelscript.moduleEntryPoint` | `""` | Ruta del script punto de entrada del módulo global. Restringe el cierre de compilación a su grafo acíclico dirigido (DAG) de dependencias hacia adelante. |
| `angelscript.searchDirectories` | `[]` | Rutas de búsqueda de directorios personalizadas para resolver archivos de script incluidos (`#include "path.as"`).  Acepta las variables `${...}` que usa `launch.json`: `${workspaceFolder}`, `${workspaceFolder:nombre}`, `${userHome}` y `${env:NOMBRE}`. VS Code no las expande en los ajustes normales, así que lo hace la extensión - una variable que esta ventana no puede responder se deja en la ruta tal como está escrita y se anota en el registro del servidor. |
| `angelscript.predefined.active` | `""` | Selecciona el único stub predefinido cargado por el barrido del espacio de trabajo.  Cuando está vacío (por defecto), el barrido carga el primer stub que encuentra en orden de ruta e ignora el resto, e indica cuál eligió. Establézcalo en `all` para cargar todos los stubs descubiertos juntos, que es lo que un espacio de trabajo con dos stubs solía hacer por defecto - las declaraciones compartidas se resuelven entonces más de una vez.  No afecta a `#angelscript.predefinedFiles`, que continúa cargándose siempre.  El comando "AngelScript: Seleccionar stub predefinido" completa esta configuración por usted.  Acepta las variables `${...}` que usa `launch.json`: `${workspaceFolder}`, `${workspaceFolder:nombre}`, `${userHome}` y `${env:NOMBRE}`. VS Code no las expande en los ajustes normales, así que lo hace la extensión - una variable que esta ventana no puede responder se deja en la ruta tal como está escrita y se anota en el registro del servidor. |
| `angelscript.predefinedFiles` | `[]` | Archivos de stubs predefinidos que declaran la API de la aplicación host, cargados por ruta. Las rutas absolutas se usan tal cual, por lo que el stub puede residir fuera del espacio de trabajo (p. ej. `C:/Games/svencoop/as.predefined`); las rutas relativas se resuelven con respecto a cada carpeta del espacio de trabajo. Los archivos dentro del espacio de trabajo cuyo nombre termina con `#angelscript.predefinedExtension` se detectan automáticamente y no necesitan una entrada aquí.  Acepta las variables `${...}` que usa `launch.json`: `${workspaceFolder}`, `${workspaceFolder:nombre}`, `${userHome}` y `${env:NOMBRE}`. VS Code no las expande en los ajustes normales, así que lo hace la extensión - una variable que esta ventana no puede responder se deja en la ruta tal como está escrita y se anota en el registro del servidor. |
| `angelscript.stubs.activeFiles` | `[]` | Lista de archivos de stubs predefinidos activos cargados en el índice del espacio de trabajo. Administrado dinámicamente mediante el comando AngelScript: Seleccionar stubs predefinidos. |
| `angelscript.rulesFile` | `""` | Ruta a un archivo personalizado *.angelscript.rules.json de reglas del motor. Si está vacío, la raíz del espacio de trabajo y los directorios de stubs predefinidos se examinan automáticamente en busca de *.angelscript.rules.json. |
| `angelscript.include.implicitExtension` | `false` | Hace que `#include "helper"` encuentre `helper.as`. Desactivado por defecto, que es el comportamiento del propio AngelScript: CScriptBuilder abre exactamente el texto entre comillas y no añade nada, así que `#include "helper"` busca un archivo llamado literalmente `helper`. Actívalo para un host que resuelve el nombre por su cuenta y exige que se omita la extensión - Sven Co-op funciona así, y ahí la forma corta es la grafía correcta. El sufijo que se añade es `angelscript.fileExtension`. |
| `angelscript.forceIncludeFiles` | `[]` | Archivos a incluir forzosamente antes de analizar cualquier módulo o script. |
| `angelscript.exclude` | `["**/.git/**", "**/build/**", "**/node_modules/**"]` | Patrones glob de directorios en los que no descienden los barridos del espacio de trabajo. Admite `?`, `*` dentro de un segmento de ruta y `**` entre segmentos. Definir esto reemplaza los valores predeterminados en lugar de añadirse a ellos. |
| `angelscript.fileExtension` | `".as"` | Sufijo de nombre de archivo de los archivos fuente de AngelScript, utilizado al realizar el barrido del espacio de trabajo para construir el grafo de `#include`. |
| `angelscript.predefinedExtension` | `".as.predefined"` | Sufijo de nombre de archivo que marca un archivo encontrado en el espacio de trabajo como un stub predefinido. Esto es un sufijo, no una ruta - use `#angelscript.predefinedFiles` para cargar un archivo específico. |
| `angelscript.define` | `[]` | Palabras que `#if` considera definidas, coincidentes con las palabras que la aplicación host pasa a `CScriptBuilder::DefineWord`. Un `#if WORD` cuya palabra no esté listada aquí se excluye de la compilación, exactamente como lo hace el preprocesador real, y no se reportan diagnósticos en su interior. Un stub predefinido puede declarar las mismas palabras con `#define WORD`, que usualmente es el mejor lugar para ellas: el stub ya describe la configuración del motor del host y viaja con él. |
| `angelscript.arrayLikeTypes` | `[]` | Nombres de tipos de plantilla cuya lista de inicializadores es una simple repetición de su tipo de elemento, tal como ocurre con `array<T>`. El tipo array predeterminado del motor siempre está incluido.  Esto es una forma abreviada. El mecanismo general es una etiqueta `/// @listpattern {repeat T}` en la clase dentro de su stub `.as.predefined`, copiada del propio registro `asBEHAVE_LIST_FACTORY` del tipo — eso también expresa patrones que esta configuración no puede, como `{repeat {string, ?}}` de `dictionary`. Use esta configuración cuando no pueda editar el stub.  De cualquier manera, tiene que indicarse en lugar de detectarse: `array<T>` y `optional<T>` se declaran de forma idéntica en un stub, y el compilador acepta `array<int> a = {1};` mientras rechaza `optional<int> o = {1};`. |

#### 3. Configuración de Inlay Hints

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.features.inlayHints` | `true` | Mostrar sugerencias en línea de nombres de parámetros y tipos deducidos. |
| `angelscript.inlayHints.parameters` | `true` | Muestra sugerencias de nombres de parámetros para llamadas a funciones y constructores. |
| `angelscript.inlayHints.variableTypes` | `true` | Muestra sugerencias de tipo deducido para variables declaradas con 'auto' o 'auto@'. |
| `angelscript.inlayHints.functionReturnTypes` | `true` | Muestra pistas de texto insertado (inlay hints) para tipos de retorno inferidos en expresiones lambda. |
| `angelscript.inlayHints.enumMemberValues` | `true` | Muestra pistas de texto insertado (inlay hints) para valores numéricos implícitos en miembros de enumeraciones. |
| `angelscript.inlayHints.parameterReferenceModifiers` | `true` | Prefija las pistas de texto de parámetros con modificadores de dirección de referencia (&out, &inout). |
| `angelscript.inlayHints.maxParameters` | `0` | Número máximo de sugerencias en línea de parámetros a mostrar para una llamada. 0 significa ilimitado (muestra todos los parámetros). |
| `angelscript.inlayHints.maxLength` | `0` | Longitud máxima de caracteres para las etiquetas de sugerencias en línea de parámetros antes de truncar con '...'. 0 significa ilimitado (nunca truncar). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `true` | Suprime las sugerencias de nombres de parámetros cuando el texto del argumento coincide exactamente con el nombre del parámetro. El valor predeterminado es true. |
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
| `angelscript.hover.showFileLocation` | `false` | Muestra la ubicación del archivo de origen al pie de los tooltips de hover de símbolos (predeterminado: false). |
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
| `angelscript.features.typeConversionChecks` | `true` | Reportar conversiones que no cuenten con un constructor, `opConv`/`opImplConv` o `opCast`/`opImplCast` que las respalde. |
| `angelscript.features.predefinedLoader` | `true` | Cargar archivos de stubs predefinidos que describen la API de la aplicación host. |
| `angelscript.features.enableCommentSuppressions` | `true` | Habilitar la supresión de diagnósticos mediante comentarios usando '// disable <CÓDIGO>' y '// enable <CÓDIGO>' (ej. '// disable W156'). |

#### AngelScript: CodeLens

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.codeLens.showOnFunctions` | `true` | Muestra CodeLens de referencias sobre declaraciones de funciones y métodos. |
| `angelscript.codeLens.showOnClassesAndInterfaces` | `true` | Muestra CodeLens de referencias sobre declaraciones de clases e interfaces. |
| `angelscript.codeLens.showOnPropertiesAndVariables` | `true` | Muestra CodeLens de referencias sobre declaraciones de variables globales y miembros. |
| `angelscript.codeLens.showOnEnums` | `true` | Muestra CodeLens de referencias sobre declaraciones de enumeraciones. |
| `angelscript.codeLens.showZeroReferences` | `true` | Muestra CodeLens de referencias incluso cuando un símbolo tiene 0 referencias. |

#### 6. Configuración de diagnósticos semánticos y análisis

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.diagnosticSeverity` | `{}` | Sobrescribe la gravedad de diagnósticos individuales, indexados por código de diagnóstico. Ejemplo: `{ "as-warn-unused-variable": "hint" }`. |
| `angelscript.diagnostics.reportUnknownTypes` | `true` | Reportar un tipo de parámetro o de retorno que no resuelva a ninguna declaración.  `void f(TypoTypeName x)` es un error de compilación — el motor responde "Identifier 'TypoTypeName' is not a data type in namespace 'TEST' or parent" — y si se deja sin reportar, se manifiesta en cambio como silencio en cada punto de llamada, porque una llamada cuyos tipos de parámetros son desconocidos tampoco se puede verificar.  También es, desde el lado del servidor, indistinguible de un tipo legítimo registrado por el motor. Desactívelo si su host registra tipos en C++ y no declara ninguno de ellos — o mejor aún, configure un archivo stub predefinido mediante `#angelscript.predefinedFiles`. |
| `angelscript.diagnostics.reportAccessorPortability` | `false` | Mostrar sugerencia sobre un accesor `get_`/`set_` escrito sin la palabra clave `property`. Dicho accesor es una propiedad bajo `asEP_PROPERTY_ACCESSOR_MODE` 2, el valor predeterminado de este servidor, pero no bajo el modo 3, que es el del propio motor. Agregar la palabra clave se acepta en ambos, por lo que la corrección rápida no puede romper una compilación funcional. Desactivada por defecto. |
| `angelscript.diagnostics.reportAccessorDisabled` | `true` | Mostrar sugerencia donde un accesor de propiedad de script se usa como una propiedad pero el host los deshabilitó (`angelscript.engine.propertyAccessorMode` 0 o 1).  Bajo cualquiera de los dos modos, el compilador omite por completo los accesores definidos por script, por lo que `c.X` respaldado por un `get_X`/`set_X` de script es rechazado — con la palabra clave `property` y sin ella. Es una sugerencia en lugar de un error, y está desactivada por defecto, porque se le está indicando al analizador lo que hace el host: de lo contrario, un host configurado incorrectamente vería errores en código que compila bien para él. No dice nada bajo los modos 2 y 3. |
| `angelscript.diagnostics.reportBoolConversion` | `true` | Mostrar sugerencia cuando se usa una clase donde se espera un `bool`, tal como `if (h)`. Bajo `asEP_BOOL_CONVERSION_MODE` 0, el valor predeterminado del motor, esto es un error de compilación incluso cuando la clase declara `opImplConv`. Una corrección rápida llama al operador de conversión explícitamente, lo cual compila bajo ambos modos. Desactivado por defecto, y completamente silencioso cuando `angelscript.engine.boolConversionMode` es 1. |
| `angelscript.diagnostics.reportMissingFuncdef` | `false` | Mostrar sugerencia cuando una posición de tipo nombra una función en lugar de un tipo, tal como `Foo@ h` donde `Foo` es una función. Un handle de función necesita un `funcdef` para nombrar su firma, y una corrección rápida declara uno a partir de los propios parámetros y tipo de retorno de la función. Desactivado por defecto, ya que el nombre podría pertenecer a un tipo del host que este analizador no puede ver. |
| `angelscript.diagnostics.reportIntegerDivision` | `false` | Mostrar sugerencia sobre `1 / 2`, que se trunca a 0 bajo el valor predeterminado del motor. Desactivado por defecto porque una base de código que busca división entera escribe exactamente esto. |
| `angelscript.diagnostics.reportPossibleNullDereference` | `true` | Advertir cuando se desreferencia un parámetro de handle, un resultado de cast de handle o un handle no inicializado sin una comprobación previa de nulo (`!is null`, retorno temprano de guarda, etc.). Activado por defecto. |
| `angelscript.diagnostics.reportUnusedFunctions` | `false` | Advertir cuando una función global, función en espacio de nombres o método privado de clase se declara en el script pero nunca se llama ni se referencia. Los puntos de entrada del host se pueden personalizar mediante `#angelscript.diagnostics.ignoredUnusedFunctions#` o reglas del motor (`.angelscript.rules.json`). |
| `angelscript.diagnostics.reportUnusedGlobalVariables` | `true` | Advertir cuando una variable global o en espacio de nombres se declara pero nunca se referencia en ningún script. Activado por defecto. |
| `angelscript.diagnostics.reportUnusedClasses` | `false` | Advertir cuando una clase se declara en el script pero nunca se instancia, hereda ni referencia. Desactivado por defecto. |
| `angelscript.diagnostics.reportUnusedMembers` | `false` | Advertir cuando una variable miembro o método de clase se declara pero nunca se accede ni se llama en ningún script. |
| `angelscript.diagnostics.ignoredUnusedFunctions` | `[]` | Nombres de funciones (cortos o calificados) exentos de advertencias de funciones sin usar (ej. `["main", "MapInit", "PluginInit"]`). |
| `angelscript.diagnostics.ignoredUnusedFunctionPatterns` | `[]` | Patrones de expresiones regulares que eximen a las funciones coincidentes de advertencias de funciones sin usar (ej. `["^On.*", "^Hook_.*"]`). |
| `angelscript.diagnostics.ignoreUnusedGlobalsWithCallInitializer` | `false` | Indica si se deben ignorar las variables globales no utilizadas inicializadas con una llamada o constructor. |
| `angelscript.diagnostics.reportHandleComparisonEquality` | `1` | Configurar la severidad para comparaciones de igualdad de handle con null (`== null`, `!= null`) en lugar de identidad de handle (`is null`, `!is null`). 0: desactivado, 1: advertencia (por defecto del compilador), 2: error. |
| `angelscript.diagnostics.missingAssetPathSeverity` | `"off"` | Severidad del diagnostico para cadenas literales que hacen referencia a archivos de recursos no encontrados. Desactivado por defecto. |

#### 7. Configuración del dialecto del motor y preprocesador (asEP_*)

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Determina si los parámetros pueden tomar referencias a tipos primitivos y por valor sin calificadores de dirección explícitos (ej. `void fn(int &val)` o `void fn(const string &msg)`).  Cuando está desactivado (valor predeterminado del motor), los parámetros por referencia en primitivos y tipos por valor requieren `&in` o `&out` (ej. `void fn(int &in val)`), y un `&` simple se trata como `&inout`, generando un error. Cuando está activado, se permiten referencias crudas estilo C++ `&` en todos los tipos.  (Corresponde a la propiedad del motor `asEP_ALLOW_UNSAFE_REFERENCES`). |
| `angelscript.engine.privatePropAsProtected` | `false` | Determina si los miembros `private` de una clase base se comportan como `protected` y pueden ser accedidos por clases derivadas (ej. `derived.m_privateField`).  Cuando está activado, las clases derivadas pueden acceder a miembros privados de clases base sin error. Cuando está desactivado (valor predeterminado del motor), acceder a un miembro privado de la base genera un error de accesibilidad.  (Corresponde a la propiedad del motor `asEP_PRIVATE_PROP_AS_PROTECTED`). |
| `angelscript.engine.disallowGlobalVars` | `false` | Determina si se prohíbe la declaración de variables globales en los scripts.  Cuando está activado, declarar cualquier variable global (ej. `int g_counter = 0;`) fuera de una función o clase genera un error de compilación. Cuando está desactivado (valor predeterminado del motor), las variables globales están permitidas.  (Corresponde a la propiedad del motor `asEP_DISALLOW_GLOBAL_VARS`). |
| `angelscript.engine.propertyAccessorMode` | `2` | Controla cómo los métodos getter y setter (`get_Prop()`, `set_Prop(val)`) se exponen como propiedades virtuales (ej. `obj.Prop`).  `2` (Cualquier miembro) — Cualquier método que coincida con `get_X()` o `set_X(...)` puede accederse como la propiedad `X`. `3` (Requiere palabra clave property) — Solo los métodos declarados explícitamente con la palabra clave `property` (ej. `int get_Prop() property`) se exponen como propiedades; de lo contrario, referenciar `obj.Prop` genera un error de miembro inexistente.  El valor predeterminado del motor es `3`; el del LSP es `2` para máxima compatibilidad.  (Corresponde a la propiedad del motor `asEP_PROPERTY_ACCESSOR_MODE`). |
| `angelscript.engine.allowMultilineStrings` | `false` | Determina si las cadenas estándar con comillas dobles `"..."` pueden abarcar varias líneas.  Cuando está desactivado (valor predeterminado del motor), un salto de línea dentro de `"..."` genera un error de compilación y las cadenas multilínea deben usar comillas triples `"""..."""`. Cuando está activado, las cadenas `"..."` normales pueden abarcar varias líneas libremente.  (Corresponde a la propiedad del motor `asEP_ALLOW_MULTILINE_STRINGS`). |
| `angelscript.engine.boolConversionMode` | `0` | Controla si las instancias y handles de clases pueden evaluarse directamente en condiciones booleanas (ej. `if (myObj)`).  `0` (No permitir conversiones de clase) — Las clases nunca pueden usarse como condiciones, incluso si declaran `opImplConv` u `opConv` a `bool`. `1` (Permitir operadores de conversión) — Las clases que implementan `opImplConv` u `opConv` retornando `bool` se aceptan en expresiones booleanas.  (Corresponde a la propiedad del motor `asEP_BOOL_CONVERSION_MODE`). |
| `angelscript.engine.useCharacterLiterals` | `0` | Controla cómo interpreta el compilador los literales de caracteres entre comillas simples `'x'`.  `0` (Cadena de un solo carácter) — `'x'` se trata como un literal de cadena de 1 carácter (predeterminado del motor), por lo que `int c = 'x'` es un error de tipos. `1` (Código de carácter entero) — `'x'` se trata como un código numérico de carácter ASCII/entero (ej. `int c = 'A'` evalúa a 65).  (Corresponde a la propiedad del motor `asEP_USE_CHARACTER_LITERALS`). |
| `angelscript.engine.disallowValueAssignForRef` | `false` | Controla si se prohíbe la asignación por valor mediante `=` en tipos de referencia.  Cuando está activado, la asignación de valor `a = b` en objetos de referencia se rechaza como error, requiriendo asignación de handle `@a = @b`. Cuando está desactivado (predeterminado del motor), `a = b` invoca el operador de copia `opAssign` del objeto.  (Corresponde a la propiedad del motor `asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE`). |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | Sintaxis permitida para argumentos con nombre en llamadas a funciones (ej. `draw(x: 10, y: 20)` frente a `draw(x = 10, y = 20)`).  `0` (Sintaxis estricta con dos puntos) — Solo se acepta `name: value` (predeterminado del motor); `name = value` es un error de sintaxis. `1` (Signo igual con advertencia) — Se acepta `name = value` pero emite una advertencia de compilación. `2` (Signo igual aceptado silenciosamente) — Tanto `name: value` como `name = value` son válidos sin diagnósticos.  (Corresponde a la propiedad del motor `asEP_ALTER_SYNTAX_NAMED_ARGS`). |
| `angelscript.engine.disableIntegerDivision` | `false` | Controla si al dividir dos enteros con `/` se realiza división en punto flotante en lugar de división entera truncada.  Cuando está activado, la división de enteros produce un float (ej. `1 / 2` evalúa a `0.5f`). Cuando está desactivado (predeterminado del motor), la división trunca hacia cero (ej. `1 / 2` evalúa a `0`).  (Corresponde a la propiedad del motor `asEP_DISABLE_INTEGER_DIVISION`). |
| `angelscript.engine.disallowEmptyListElements` | `false` | Determina si omitir elementos con comas vacías en listas de inicialización (ej. `{1, , 3}`) se considera un error.  Cuando está activado, las posiciones omitidas en listas inicializadoras generan un error de compilación. Cuando está desactivado (predeterminado del motor), se permiten huecos vacíos y se inicializan con valores por defecto.  (Corresponde a la propiedad del motor `asEP_DISALLOW_EMPTY_LIST_ELEMENTS`). |
| `angelscript.engine.foreachSupport` | `true` | Controla si los bucles de iteración `foreach` (ej. `foreach (auto item : container)`) están soportados.  Cuando está activado (predeterminado del motor), los bucles `foreach` compilan con normalidad. Cuando está desactivado, encontrar una sentencia `foreach` genera un error de sintaxis o característica no admitida.  (Corresponde a la propiedad del motor `asEP_FOREACH_SUPPORT`). |
| `angelscript.engine.requireEnumScope` | `false` | Controla si los valores de una enumeración deben calificarse siempre con el nombre del tipo enum (ej. `Color::Red` frente a `Red` simple).  Cuando está activado, los nombres de enumeradores no están visibles en el ámbito circundante y escribir `Red` genera un error de símbolo no resuelto a menos que se escriba `Color::Red`. Cuando está desactivado (predeterminado del motor), los valores pueden usarse directamente sin calificación de ámbito.  (Corresponde a la propiedad del motor `asEP_REQUIRE_ENUM_SCOPE`). |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | Controla si las clases proporcionan automáticamente un constructor por defecto sin argumentos cuando se definen constructores personalizados.  Cuando está activado (predeterminado del motor), siempre se genera un constructor implícito `MyClass()`, permitiendo `MyClass obj;` aunque exista `MyClass(int a)`. Cuando está desactivado, definir cualquier constructor impide la construcción por defecto salvo que se declare un constructor explícito `MyClass()`.  (Corresponde a la propiedad del motor `asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`). |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | Determina si se permiten caracteres Unicode y no ASCII en nombres de identificadores y símbolos (ej. `int счётчик = 0;` o `void calcular_año()`).  Cuando está activado, letras y dígitos UTF-8 no ASCII son válidos en nombres de variables, funciones y clases. Cuando está desactivado (predeterminado del motor), los caracteres no ASCII en identificadores generan errores de sintaxis.  (Corresponde a la propiedad del motor `asEP_ALLOW_UNICODE_IDENTIFIERS`). |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | Determina si declarar la misma interfaz compartida en múltiples archivos de script compila limpiamente sin errores de declaración duplicada.  Cuando está activado, declaraciones idénticas de `shared interface IFoo` en diferentes módulos o archivos se unifican sin error. Cuando está desactivado (predeterminado del motor), redeclarar la misma interfaz compartida genera un diagnóstico de conflicto de nombres.  (Corresponde a la propiedad del motor `asEP_IGNORE_DUPLICATE_SHARED_INTF`). |
| `angelscript.engine.compilerWarnings` | `1` | Controla cómo el motor y el servidor de lenguaje gestionan las advertencias del compilador.  `0` (Suprimir advertencias) — Desactiva por completo la emisión de advertencias de compilación. `1` (Emitir advertencias) — Notifica las advertencias de compilación como diagnósticos de tipo advertencia en el LSP (predeterminado del motor). `2` (Tratar advertencias como errores) — Promueve las advertencias a errores de compilación, impidiendo la compilación correcta.  (Corresponde a la propiedad del motor `asEP_COMPILER_WARNINGS`). |
| `angelscript.engine.noHandleTypes` | `[]` | Nombres de tipos por valor registrados por el host que no admiten handles de objeto (`@`), como `string` o `datetime`.  Se combina (unión de conjuntos) con el arreglo de nivel superior `noHandleTypes` en `angelscript.rules.json`. Declarar un handle a cualquier tipo listado (p. ej., `string@ s;` o `array<string@>`) reporta `as-err-handle-on-no-handle-type` (`E261`). |
| `angelscript.preprocessor.elseSupport` | `false` | Active esto si el host aplicó un parche a su copia de `scriptbuilder.cpp` para comprender `#else`.  El complemento incluido con el SDK no lo hace. Medido frente al compilador real: en `#if FOO / a / #else / b / #endif` con `FOO` sin definir, la rama `#else` **no** se convierte en la tomada — se borra junto con el resto del bloque, porque `#else` no es una directiva y la exclusión se extiende hasta `#endif` de todos modos. Con esto desactivado, eso es exactamente lo que este servidor asume. |
| `angelscript.preprocessor.elifSupport` | `false` | Active esto si el host aplicó un parche a su copia de `scriptbuilder.cpp` para comprender `#elif`.  No está presente en el complemento de serie. Con esto activado, la primera rama cuya palabra esté definida es la activa y todas las demás ramas quedan excluidas. |
| `angelscript.preprocessor.ifdefSupport` | `false` | Active esto si el host aplicó un parche a su copia de `scriptbuilder.cpp` para comprender `#ifdef` y `#ifndef`.  No está presente en el complemento de serie, donde cualquiera de los dos se deja en el código fuente y el compilador reporta `Unexpected token`. |
| `angelscript.preprocessor.defineInScripts` | `false` | Active esto si el host aplicó un parche a su copia de `scriptbuilder.cpp` para que `#define WORD` en un script defina una palabra.  No está presente en el complemento de serie, donde `DefineWord` es una llamada de C++ que realiza el host y un `#define` escrito en un script es un error de sintaxis. Para declarar las palabras que el propio host define, use `#angelscript.define` o una línea `#define` en un stub predefinido, que no se ven afectados por esta configuración. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | Qué reportar para un `#pragma`.  El complemento de serie los rechaza todos: al no haber ningún callback de pragma registrado, sustituye la respuesta del callback por un fallo, escribe `Invalid #pragma directive` y hace fallar toda la sección. Sin embargo, el valor predeterminado aquí es `accept`, porque un host que registra un callback es el caso habitual y reportar un error por defecto pondría un subrayado ondulado en un pragma que compila bien. Elija `error` para un host que realmente no haya registrado nada, o `hint` si no está seguro. |
| `angelscript.metadata.enabled` | `false` | Habilita el soporte para bloques de metadatos de CScriptBuilder ('[...]') antes de declaraciones de script (clases, interfaces, funciones, variables). |

#### AngelScript: Depurador

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.debug.enableBreakpointLocations` | `true` | Habilitar la verificación semántica de ubicaciones de puntos de interrupción mediante el servidor AngelLSP antes de vincularlos. |
| `angelscript.debug.defaultPort` | `9002` | Puerto TCP predeterminado para conectar al servidor de depuración en tiempo de ejecución de AngelScript (predeterminado: 9002). |
| `angelscript.debug.defaultProtocol` | `"dap"` | Protocolo predeterminado para depuración de AngelScript ('dap' para Protocolo de Adaptador de Depuración, 'asdbg' para socket binario o 'cdebugger' para socket de comandos de texto). |
<!-- SETTINGS_CATALOG_END -->

---

Ver también: [README](README.es.md) | [Guía de reglas del motor](rules.es.md) | [CHANGELOG.md](../CHANGELOG.md)
