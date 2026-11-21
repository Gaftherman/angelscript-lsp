# Guía detallada de Angelscript

[English](GUIDE.md) | [Español](GUIDE.es.md) | [README](README.es.md)

Empieza por el ejemplo que más se parezca a cómo tu juego carga los scripts. Después ajusta la API y las reglas del lenguaje que verá VS Code.

## Ejemplos de configuración del espacio de trabajo

Pon el ejemplo en `.vscode/settings.json`, dentro de `${workspaceFolder}`. En `angelscript.modules`, usa `folder` para agrupar todo lo que hay bajo una carpeta o `entry` para partir de un script y seguir sus `#include`.

Con los módulos definidos, el editor puede comprobar si una declaración `external shared` tiene su definición en otro módulo.

### Ejemplo A: Sven Co-op (módulo por carpeta)

Si todos los scripts de `maps/hcas` se compilan juntos, asigna un módulo a esa carpeta.

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

Si tu juego empieza por `ins2_register.as`, pon ese archivo como punto de entrada. Los scripts que incluye quedan en el mismo módulo.

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

Si tu motor guarda los archivos de inclusión y las declaraciones de la API en carpetas distintas, indica cada ruta por separado.

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

Declara en un stub `.as.predefined` la API que tu juego registra desde C++. Así el editor puede sugerir las clases y funciones del juego, mostrar sus firmas en el hover y revisar cómo las usas.

### Cómo se cargan los stubs

- La búsqueda en el workspace usa el sufijo de `angelscript.predefinedExtension`. El valor inicial es `.as.predefined`.
- Elige un stub con `angelscript.predefined.active`. Si lo dejas vacío, se carga el primero por orden de ruta. Con `"all"`, se cargan todos los que se encontraron.
- Agrega a `angelscript.predefinedFiles` los stubs que quieras cargar siempre. Pueden estar fuera de `${workspaceFolder}` y sus rutas pueden usar `${userHome}` o `${env:NOMBRE}`.
- Si prefieres elegirlos desde VS Code, busca **Seleccionar stub predefinido** o **Seleccionar stubs predefinidos** en la paleta de comandos. También puedes abrir el menú de acciones desde el indicador de estado **AngelScript IntelliSense**.

Puedes tomar como ejemplo el stub de Sven Co-op 5.26 en [predefined/sven.as.predefined](../predefined/sven.as.predefined).

## Reglas del motor (`*.angelscript.rules.json`)

Si el motor llama a `MapInit` desde C++, agrégalo a las reglas para que el editor deje de marcarlo como función sin usar. Las reglas también sirven para asociar `BaseClass` con una clase nativa del stub o señalar llamadas y declaraciones que tu motor restringe.

- Guarda `angelscript.rules.json` en la raíz del workspace para que se cargue automáticamente. El servidor también busca archivos `*.angelscript.rules.json` junto a los stubs y en la raíz.
- Para elegir un archivo concreto, indica su ruta con `angelscript.rulesFile` en `.vscode/settings.json`.
- Para crear un archivo inicial, abre **Configurar reglas del motor y API predefinida...** desde la paleta de comandos y elige la opción para crear las reglas.

Consulta los ejemplos de la [Guía de reglas del motor](RULES.es.md) y el archivo [predefined/sven.angelscript.rules.json](../predefined/sven.angelscript.rules.json).

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

Ajusta las llaves y los espacios con `angelscript.format.*`. Al formatear, el editor busca `.clang-format`, `_clang-format` o `.as-clang-format` en la carpeta del archivo y en las carpetas superiores. Si encuentra opciones compatibles, las aplica por encima de los ajustes equivalentes del editor.

Puedes usar `BasedOnStyle`, secciones `Language: AngelScript` o `Cpp`, y opciones como `SpaceBeforeParens`, `PointerAlignment` y `AllowShortBlocksOnASingleLine`. Si quieres conservar el formato de un bloque, rodéalo con `// clang-format off` y `// clang-format on`.

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

Usa `type: "angelscript"` en la configuración de depuración de VS Code para poner puntos de interrupción y revisar la pila de llamadas y las variables locales. Elige `dap`, `asdbg` o `cdebugger` con `angelscript.debug.defaultProtocol` y ajusta `angelscript.debug.defaultPort` al puerto del motor. El valor inicial es `9002`.

## Compilar desde el código fuente

Para compilar el servidor necesitas un compilador C++20 y CMake 3.22 o posterior. Para el cliente necesitas Node.js y `npm`. Ejecuta estos comandos desde la raíz del repositorio.

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile
```

Consulta la [guía de configuración](CONFIGURATION.es.md) para ver todos los ajustes y ejemplos para editores.

---

[README](README.es.md) | [Guía de reglas del motor](RULES.es.md) | [CHANGELOG.md](../CHANGELOG.md)
