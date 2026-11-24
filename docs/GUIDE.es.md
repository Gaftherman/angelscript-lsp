# Guía detallada de Angelscript

[English](GUIDE.md) | [Español](GUIDE.es.md) | [README](README.es.md)

> **Palabras que verás**
>
> - El **host** es el juego o programa que ejecuta tus scripts.
> - La **API** son los nombres que ofrece el host. Un **stub** los enumera en un archivo como `game.as.predefined`.
> - Un **módulo con script de entrada** reúne un script inicial, como `main.as`, y todos los archivos a los que llega por `#include`.
> - Una **inclusión** es una directiva como `#include "helper.as"` que incorpora otro archivo al módulo.
> - Un **diagnóstico** es un mensaje del editor, como un aviso sobre una variable sin usar.

## Ejemplos de configuración del espacio de trabajo

### Empieza con un solo archivo

```text
└── 📁my-project
    ├── 📁.vscode
    │   └── 📄settings.json
    └── 📄main.as
```

Abre `my-project` en VS Code y escribe `int main() { return 42; }` en `main.as`. Pasa el cursor sobre `main` para ver `int main()`. Para este ejemplo puedes prescindir de `.vscode/settings.json` y de los stubs.

Pon el ejemplo de motor que corresponda a tu proyecto en `.vscode/settings.json`, dentro de `${workspaceFolder}`. En `angelscript.modules`, usa `folder` para agrupar todo lo que hay bajo una carpeta o `entry` para partir de un script y seguir sus `#include`. Con los módulos definidos, el editor puede comprobar si una declaración `external shared` tiene su definición en otro módulo.

### Ejemplo A: Carpeta de plugins del juego (módulo por carpeta)

Si todos los scripts de `maps/items` se compilan juntos, asigna un módulo a esa carpeta.

```text
└── 📁my-game-scripts
    ├── 📁.vscode
    │   └── 📄settings.json
    └── 📁maps
        ├── 📁items
        │   ├── 📄main.as
        │   └── 📄inventory.as
        └── 📄game.as.predefined
```

```jsonc
{
  "angelscript.include.implicitExtension": true,
  "angelscript.searchDirectories": ["${workspaceFolder}/maps"],
  "angelscript.modules": [
    {
      "name": "Items",
      "folder": "${workspaceFolder}/maps/items"
    }
  ],
  "angelscript.predefined.active": "${workspaceFolder}/maps/game.as.predefined"
}
```

### Ejemplo B: Carpeta de plugins del juego (módulo por script de entrada)

Si tu juego empieza por `register.as`, pon ese archivo como punto de entrada. Los scripts que incluye quedan en el mismo módulo.

```text
└── 📁my-game-scripts
    ├── 📁.vscode
    │   └── 📄settings.json
    └── 📁maps
        ├── 📁events
        │   └── 📄register.as
        └── 📄game.as.predefined
```

```jsonc
{
  "angelscript.include.implicitExtension": true,
  "angelscript.searchDirectories": ["${workspaceFolder}/maps"],
  "angelscript.modules": [
    {
      "name": "MapInit",
      "entry": "${workspaceFolder}/maps/events/register.as"
    }
  ],
  "angelscript.predefined.active": "${workspaceFolder}/maps/game.as.predefined"
}
```

### Ejemplo C: Motor genérico o proyecto independiente

Si tu motor guarda los archivos de inclusión y las declaraciones de la API en carpetas distintas, indica cada ruta por separado.

```text
└── 📁game-project
    ├── 📁.vscode
    │   └── 📄settings.json
    └── 📁scripts
        ├── 📁api
        │   └── 📄engine.as.predefined
        ├── 📁game
        │   └── 📄main.as
        └── 📁include
            └── 📄utility.as
```

```jsonc
{
  "angelscript.searchDirectories": ["${workspaceFolder}/scripts/include"],
  "angelscript.modules": [
    {
      "name": "GameCore",
      "folder": "${workspaceFolder}/scripts/game"
    }
  ],
  "angelscript.predefinedFiles": ["${workspaceFolder}/scripts/api/engine.as.predefined"]
}
```

## Archivos de API predefinidos (`.as.predefined`)

Añade `game.as.predefined` junto a `main.as` y declara `int HostScore();`. En `main.as`, escribe `int main() { return HostScore(); }`. Pasa el cursor sobre `HostScore` en la llamada para ver la firma `int HostScore()` del stub.

### Cómo se cargan los stubs

- La búsqueda en el workspace usa el sufijo de `angelscript.predefinedExtension`. El valor inicial es `.as.predefined`.
- Usa `angelscript.predefined.active` para elegir uno de los stubs detectados y `angelscript.predefinedFiles` para cargar archivos adicionales por ruta, incluso si están fuera del espacio de trabajo.
- Si dejas `angelscript.predefined.active` vacío, se carga el primer stub detectado por orden de ruta. Usa `"all"` para cargarlos todos.
- Si prefieres elegirlos desde VS Code, busca **Seleccionar stub predefinido** o **Seleccionar stubs predefinidos** en la paleta de comandos. También puedes abrir el menú de acciones desde el indicador de estado **AngelScript IntelliSense**.

## Reglas del motor (`angelscript.rules.json`)

Si el motor llama a `MapInit` desde C++, agrégalo a las reglas para que el editor deje de marcarlo como función sin usar. Las reglas también sirven para asociar `BaseClass` con una clase nativa del stub o señalar llamadas y declaraciones que tu motor restringe.

El servidor carga un solo archivo de reglas. Primero busca el archivo indicado por `angelscript.rulesFile`. Si no existe, busca `angelscript.rules.json` en una raíz del espacio de trabajo, luego junto al stub activo, después junto a los demás stubs conocidos, y por último un `*.angelscript.rules.json` en una raíz. Junto a `game.as.predefined` prueba `game.angelscript.rules.json` antes de elegir la primera coincidencia del patrón por orden de ruta.

Ajusta el editor a tu host y usa un archivo de reglas para compartir opciones y comprobaciones. Los valores de `engineProperties` del archivo cargado tienen prioridad. Por ejemplo, `"requireEnumScope": true` en el archivo prevalece sobre `false` en el editor. El servidor combina ambas listas de `noHandleTypes`.

Para crear un archivo inicial, abre **Configurar reglas del motor y API predefinida...** desde la paleta de comandos y elige la opción para crear las reglas.

Consulta los ejemplos de la [Guía de reglas del motor](RULES.es.md).

Para ver un ejemplo real de Sven Co-op, consulta `predefined/sven.as.predefined` y `predefined/sven.angelscript.rules.json`.

## Opciones del motor y qué cambia en el editor

Usa los mismos valores que tu motor usa para las opciones `asEP_*` y el preprocesador. Los valores de las tablas son los que trae la extensión.

### Opciones de lenguaje y sintaxis (`angelscript.engine.*`)

| Configuración | Por defecto | Qué cambia en el editor |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Ponlo en `true` si tu motor acepta `&` sin dirección en parámetros de tipos primitivos o por valor, como `int &x`. Con `false`, indica una dirección como `&in` o `&out`. |
| `angelscript.engine.privatePropAsProtected` | `false` | Con `true`, una clase derivada puede acceder a miembros `private` de su clase base. |
| `angelscript.engine.disallowGlobalVars` | `false` | Con `true`, el editor marca las declaraciones de variables globales. |
| `angelscript.engine.propertyAccessorMode` | `2` | Con `2`, puedes usar `obj.X` si hay un `get_X()` o `set_X(v)`. Con `3`, agrega `property` al accesor para usar `obj.X`. |
| `angelscript.engine.allowMultilineStrings` | `false` | Con `true`, las cadenas entre comillas dobles pueden ocupar varias líneas. Con `false`, usa comillas triples (`"""..."""`) para una cadena multilínea. |
| `angelscript.engine.boolConversionMode` | `0` | Con `1`, `if (obj)` puede usar el `opImplConv` u `opConv` a `bool` de la clase. Con `0`, escribe la conversión de forma explícita. |
| `angelscript.engine.useCharacterLiterals` | `0` | Con `0`, `'A'` es una cadena y asignarla a `int` da un error de tipo. Con `1`, se interpreta como el código numérico del carácter. |
| `angelscript.engine.disallowValueAssignForRef` | `false` | Con `true`, el editor marca `a = b` si el objeto es de un tipo por referencia. Para reasignar el handle, escribe `@a = @b`. |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | `name: value` funciona en los tres modos. Para `name = value`, `0` muestra un error, `1` da un aviso y `2` lo acepta. |
| `angelscript.engine.disableIntegerDivision` | `false` | Con `false`, `1 / 2` tiene tipo entero. Con `true`, el editor trata la división entre enteros como una expresión de punto flotante. |
| `angelscript.engine.disallowEmptyListElements` | `false` | Con `true`, el editor marca el hueco de `{1, , 3}`. |
| `angelscript.engine.foreachSupport` | `true` | Con `false`, el editor marca `foreach` como una instrucción que el motor no admite. |
| `angelscript.engine.requireEnumScope` | `false` | Con `true`, escribe `Color::Red`. El editor marca `Red` si aparece sin el nombre del enum. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | Con `true`, una clase con `Item(int id)` recibe también un constructor implícito sin argumentos. Con `false`, declara `Item()` si quieres escribir `Item x;`. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | Con `true`, el editor acepta letras fuera de ASCII en los identificadores. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | Con `true`, puedes repetir una declaración idéntica de `shared interface` en varios archivos sin que el editor marque un duplicado. |
| `angelscript.engine.compilerWarnings` | `1` | `0` oculta los avisos del compilador, `1` los muestra como advertencias y `2` los muestra como errores. |
| `angelscript.engine.noHandleTypes` | `[]` | Incluye los tipos que tu motor registra sin soporte para handles, como `["string", "datetime"]`. Un handle a cualquiera de esos tipos produce `as-err-handle-on-no-handle-type` (`E261`). La lista se combina con `noHandleTypes` de `angelscript.rules.json`. |

### Opciones de preprocesador y metadatos

| Configuración | Por defecto | Qué cambia en el editor |
| :--- | :--- | :--- |
| `angelscript.preprocessor.elseSupport` | `false` | Con `true`, `#else` puede activar la otra rama si `#if` es falso. Con `false`, un `#if` falso excluye todo hasta `#endif`. |
| `angelscript.preprocessor.elifSupport` | `false` | Con `true`, el editor analiza la primera rama que coincida en una cadena con `#elif`. |
| `angelscript.preprocessor.ifdefSupport` | `false` | Con `true`, `#ifdef` y `#ifndef` eligen ramas según las palabras definidas, por ejemplo con `angelscript.define` o un `#define` del stub. |
| `angelscript.preprocessor.defineInScripts` | `false` | Con `true`, un `#define WORD` en una parte activa del script puede cambiar qué ramas de `#if` se analizan más adelante. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | Para `#pragma`, `"accept"` no muestra ningún diagnóstico, `"hint"` da una sugerencia y `"error"` marca un error. |
| `angelscript.metadata.enabled` | `false` | Con `true`, el editor acepta bloques de metadatos `[...]` antes de clases, interfaces, funciones y variables. |

## Formato y `.clang-format`

Usa **Dar formato al documento** para todo el script o **Dar formato a la selección** para las líneas seleccionadas, por ejemplo, las de una función. Si activas el formato al guardar manualmente o al escribir, se busca el estilo de la misma forma. **Formatear stub predefinido** usa su propio formateador y agrupa las declaraciones por espacio de nombres.

Si el archivo existe, la búsqueda empieza en su carpeta y sube hasta la raíz del sistema de archivos. En cada carpeta prueba `.clang-format`, `_clang-format` y después `.as-clang-format`. Por ejemplo, para un script en `scripts/`, se elige `scripts/_clang-format` antes que un `.clang-format` en la raíz del proyecto. La búsqueda termina en el primer archivo normal que se pueda abrir.

Si la ruta no existe o no se carga un estilo, se usan las opciones de sangría y espacios del editor junto con `angelscript.format.*`. Al guardar manualmente, el formato parte de cuatro espacios. El estilo de llaves predeterminado, `allman`, pone la llave de apertura en su propia línea si el cuerpo de la función tiene contenido.

Separa las secciones de idioma con `---`. Se elige primero `Language: AngelScript`, después `Language: Cpp` y por último la sección sin idioma. Cada sección de idioma hereda los ajustes sin etiqueta que aparecen antes. Pon un estilo base como `BasedOnStyle: Google` antes de tus propios ajustes.

| Claves | Ejemplo |
| :--- | :--- |
| `IndentWidth`, `UseTab`, `NamespaceIndentation`, `BreakBeforeBraces` | `IndentWidth: 2` con `UseTab: Never` aplica una sangría de dos espacios. |
| `SpaceBeforeParens`, `SpacesInParentheses`, `PointerAlignment`, `ReferenceAlignment` | `SpaceBeforeParens: Always` deja la llamada como `draw (x)`. |
| `AllowShortBlocksOnASingleLine`, `MaxEmptyLinesToKeep` | `Empty` conserva un bloque vacío que ya está en una sola línea. `MaxEmptyLinesToKeep: 1` limita las líneas en blanco consecutivas al formatear todo el script. |

El formateador ignora `ColumnLimit`, `TabWidth`, las subclaves de `BraceWrapping` y opciones como `AllowShortFunctionsOnASingleLine`. Por ejemplo, con `ColumnLimit: 80` las líneas largas quedan sin dividir.

Pon `// clang-format off` y `// clang-format on` en líneas propias para delimitar un bloque. Si falta el `on`, la pausa dura hasta el final del archivo. Las tabulaciones, las líneas en blanco y los espacios al final pueden cambiar dentro del bloque. Pon `DisableFormat: true` en el estilo elegido para omitir el formato de todo el archivo.

## Funciones de uso diario

Pon estos ajustes en `.vscode/settings.json`. Consulta la [guía de configuración](CONFIGURATION.es.md).

- **Cambiar nombre**. Usa **Cambiar nombre de símbolo** sobre `spawn` para actualizar su declaración y sus usos en los scripts indexados. Los nombres del host declarados solo en un stub, como `RandomFloat`, son de solo lectura.
  Pon `angelscript.features.rename` en `false` para desactivar la función.
- **Referencias**. Haz clic derecho en `spawn` y elige **Buscar todas las referencias** para ver dónde se usa.
  Pon `angelscript.features.references` en `false` para desactivar la función.
- **Acciones de código**. Abre la bombilla para ver acciones como **Eliminar variable no utilizada** en una variable local sin usar o **Extraer variable** en una expresión apta.
  Pon `angelscript.features.codeAction` en `false` para ocultar estas acciones.
- **Ayuda de firma**. Escribe `draw(` para ver los nombres y tipos de los parámetros. En `draw(10,`, la coma puede resaltar el parámetro siguiente.
  Pon `angelscript.features.signatureHelp` en `false` para ocultar la ayuda.
- **CodeLens**. Haz clic en `2 referencias` encima de `spawn` para ver sus usos si las referencias están activadas. Las interfaces y sus métodos pueden mostrar etiquetas con el número de implementaciones.
  Pon `angelscript.features.codeLens` en `false` para ocultar los recuentos. `angelscript.codeLens.showOnEnums` controla los recuentos en miembros como `Red` en `enum Color { Red }`.
- **Pistas en línea**. Ve los nombres de los parámetros junto a los argumentos. Si `draw` declara el parámetro `int x`, la llamada `draw(10)` puede mostrar `x:` junto a `10`.
  Pon `angelscript.features.inlayHints` en `false` para ocultar las etiquetas.
- **Resaltado semántico**. Distingue los parámetros y los campos por su color según el papel que cumplen.
  Pon `angelscript.features.semanticTokens` en `false` para quedarte con el resaltado habitual de la gramática.
- **Diagnósticos**. Lee el código junto al mensaje en **Problemas** o al pasar el cursor sobre el subrayado. Para una variable local sin usar, pon `"angelscript.diagnosticSeverity": {"W103": "hint"}`. También puedes usar el código largo `as-warn-unused-variable`.
  Para desactivar una comprobación concreta, pon su ajuste en `false`, por ejemplo, `angelscript.diagnostics.reportPossibleNullDereference`.

El servidor envía los diagnósticos de forma automática. En el modo bajo demanda, el editor los solicita. Inicia el servidor con `--enable-pull-diagnostics` si tu cliente admite este modo.

**Ir a la definición** e **Ir a la declaración** usan la misma búsqueda. **Ir a la definición de tipo** lleva al tipo, como `Player` para una variable de ese tipo. Un moniker es un identificador del proyecto que el servidor devuelve al editor, como `Game::spawn`.

## Variables de ruta

Puedes usar estas variables en ajustes que reciben rutas, como `angelscript.searchDirectories`.

| Variable | Significado |
| :--- | :--- |
| `${workspaceFolder}` | Carpeta raíz del proyecto abierto. |
| `${workspaceFolder:name}` | Carpeta raíz de un proyecto concreto en un espacio multi-raíz. |
| `${userHome}` | Carpeta personal del usuario actual. |
| `${env:NOMBRE}` | Valor de la variable de entorno `NOMBRE`. |

## Comandos disponibles

Abre la paleta de comandos (`Ctrl+Shift+P` o `Cmd+Shift+P`). En el Explorador también hay comandos para configurar módulos y dar formato a los stubs.

| Comando | Título | Qué hace |
| :--- | :--- | :--- |
| `angelscript.configureHostEnvironment` | AngelScript: Configurar reglas del motor y API predefinida... | Abre el menú para configurar los stubs o las reglas del motor. |
| `angelscript.ignoreUnusedSymbol` | AngelScript: Ignorar símbolo no usado (marcar como punto de entrada / callback) | Agrega la función o el callback a las reglas para que deje de aparecer como símbolo sin usar. |
| `angelscript.selectPredefined` | AngelScript: Seleccionar stub predefinido | Elige el stub activo. |
| `angelscript.selectStubs` | AngelScript: Seleccionar stubs predefinidos | Selecciona varios stubs para cargarlos juntos. |
| `angelscript.rescanWorkspace` | AngelScript: Volver a examinar el espacio de trabajo | Vuelve a buscar los scripts y stubs del workspace. |
| `angelscript.statusMenu` | AngelScript: Menu de estado de AngelScript | Abre el menú de acciones para módulos y stubs. |
| `angelscript.switchActiveModule` | AngelScript: Cambiar módulo activo | Elige el módulo activo. |
| `angelscript.showServerLog` | AngelScript: Mostrar registro del servidor | Muestra la salida del servidor de lenguaje en VS Code. |
| `angelscript.openLogsFolder` | AngelScript: Abrir carpeta de registros | Abre la carpeta de registros del servidor. |
| `angelscript.restartServer` | AngelScript: Reiniciar servidor | Reinicia el servidor de lenguaje. |
| `angelscript.setModuleEntryPoint` | Definir como punto de entrada del módulo | Haz clic derecho en un archivo `.as` para usarlo como punto de entrada de un módulo. |
| `angelscript.setModuleFolder` | Definir como carpeta del módulo | Haz clic derecho en una carpeta para asignar sus scripts a un módulo. |
| `angelscript.formatPredefinedStub` | Formatear stub predefinido | Haz clic derecho en un archivo `.predefined` para formatear sus declaraciones. |
| `angelscript.clearLocalState` | Limpiar anulaciones de sesión local | Borra las selecciones locales de módulos y stubs y recupera los ajustes del workspace. |

## Depurador

Con el depurador de VS Code puedes usar puntos de interrupción, ver la pila de llamadas e inspeccionar variables mientras tu aplicación ejecuta los scripts.
El [proyecto de ejemplo del depurador](https://github.com/Gaftherman/angelscript-debugger-example) muestra cómo configurar la aplicación y realizar una sesión de depuración completa.

Con una configuración de tipo `launch`, pulsa F5 para iniciar la aplicación anfitriona y ver su salida en la consola de depuración de VS Code, tal como se muestra en el proyecto de ejemplo.
Para ejecutar un script sin un juego, instala la [CLI de AngelScript (`angelscript` / `asc`)](https://github.com/Gaftherman/angelscript-cli) y configura `angelscript.runner.path` o añade el ejecutable a tu `PATH`.

<p align="center"><img src="https://raw.githubusercontent.com/Gaftherman/angelscript-debugger-example/main/media/debugger.gif" alt="Ejecución paso a paso de AngelScript en VS Code" width="720"></p>
<p align="center"><em>Ejecuta un script de AngelScript paso a paso en VS Code.</em></p>

## Compilar desde el código fuente

Para compilar el servidor necesitas un compilador C++20 y CMake 3.22 o posterior. El cliente usa Node.js y `npm`. El paso de empaquetado genera `client/dist/extension.js`, que es el archivo que carga VS Code. Ejecuta estos comandos desde la raíz del repositorio.

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile && npm run bundle
```

Consulta la [guía de configuración](CONFIGURATION.es.md) para ver todos los ajustes y ejemplos para editores.

---

[README](README.es.md) | [Guía de reglas del motor](RULES.es.md) | [CHANGELOG.md](../CHANGELOG.md)
