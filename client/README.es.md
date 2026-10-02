# Angelscript - Servidor de Lenguaje para Angelscript (Extensión de VS Code)

**[English](README.md)** | **[Español](README.es.md)**

Angelscript ofrece soporte de lenguaje avanzado para [AngelScript](https://www.angelcode.com/angelscript/) (`.as`), impulsado por un servidor de lenguaje nativo en C++20 que utiliza Tree-Sitter para el análisis de AST y la resolución semántica. Todo el espacio de trabajo se analiza directamente a partir de árboles sintácticos sin concatenación de scripts, volcados intermedios en disco ni ejecución en el motor host.

---

## Guía de inicio rápido

### 1. Instalación

Instala la extensión desde el [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman) o busca `Angelscript` por Gaftherman en la vista de Extensiones (`Ctrl+Shift+X`):
1. Abre Visual Studio Code.
2. Presiona `Ctrl+P`, pega `ext install Gaftherman.angelscript-gaftherman` y presiona Enter.

Alternativamente, para instalar desde un paquete `.vsix` compilado:
1. Presiona `Ctrl+Shift+P` (o `Cmd+Shift+P` en macOS) y ejecuta `Extensions: Install from VSIX...`.
2. Selecciona el paquete compilado de la extensión (`angelscript.vsix` o `angelscript-*.vsix`).

### 2. Configuración del espacio de trabajo

Abre tu carpeta de trabajo en VS Code. Configura `.vscode/settings.json` de acuerdo con la estructura de tu motor:

#### Ejemplo A: Sven Co-op (Módulo por carpeta)
Recomendado para carpetas de scripts de mapas en Sven Co-op (ej. `maps/hcas`):
```jsonc
{
  // Permite inclusiones sin extensión (ej. #include "helper" resuelve a helper.as)
  "angelscript.include.implicitExtension": true,

  // Directorios adicionales de búsqueda para rutas de #include relativas o entre corchetes angulares
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
Recomendado cuando se compila un árbol de scripts comenzando desde un punto de registro:
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

#### Ejemplo C: Motor de videojuegos genérico / Espacio independiente
Para integraciones de host personalizadas con rutas de inclusión y stubs de API propios:
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

En AngelScript, las aplicaciones host registran sus APIs en C++ (clases, funciones globales, propiedades y constantes) dentro del motor de scripting en tiempo de ejecución. Para proporcionar IntelliSense preciso, autocompletado, validación de tipos y navegación para estas APIs del host, AngelLSP carga encabezados de stubs `.as.predefined`.

Están disponibles los siguientes stubs comunitarios probados:

| Entorno del Host | Fuente y Enlace | Estado y Recomendación |
| :--- | :--- | :--- |
| **Sven Co-op** | [Sven Co-op - Gaftherman](https://github.com/Gaftherman/angelscript-lsp/blob/main/predefined/sven.as.predefined) | **Recomendado (Recommended)** — Mantenido activamente y actualizado para las vinculaciones modernas de Sven Co-op 5.26+, cualificadores const, calificadores de referencia y estructuras completas de matemáticas/motor. Recomendamos ampliamente este stub para todo desarrollo en Sven Co-op. |
| **Sven Co-op** | [Sven Co-op - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Sven%20Co-op/as.predefined) | **Heredado (Legacy)** — Conservado para retrocompatibilidad con proyectos y configuraciones antiguas; desactualizado respecto a las versiones modernas del motor. |
| **Trackmania Nations Forever** | [Trackmania Nations Forever - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Trackmania%20Nations%20Forever/as.predefined) | Compatible — Vinculaciones de la API del host para scripting en Trackmania Nations Forever (`CGameCtnApp`, `MwFastBuffer`, etc.). |
| **OpenSiv3D** | [OpenSiv3D - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/OpenSiv3D/as.predefined) | Compatible — Vinculaciones de la API del framework de juegos en C++ OpenSiv3D (`Vec2`, `ColorF`, `Circle`, etc.). |

> [!TIP]
> Para configurar un stub activo en tu espacio de trabajo, define `"angelscript.predefined.active": "${workspaceFolder}/ruta/al/stub.as.predefined"` o usa `"all"` para combinar múltiples stubs. Todos los stubs están verificados para un análisis rápido y sin errores.

---

## Internacionalización y Localización (i18n / l10n)

AngelLSP proporciona soporte bilingüe nativo tanto en **inglés** como en **español**:

- **Sincronización automática de idioma**: La extensión se adapta automáticamente al idioma configurado en VS Code (`Configurar idioma de la pantalla` en la Paleta de comandos).
- **Interfaz y configuraciones de la extensión**: Las 112 configuraciones, títulos de comandos, elementos de la barra de estado y diálogos de notificación están localizados nativamente vía `@vscode/l10n` (`bundle.l10n.json` y `bundle.l10n.es.json`) y tablas de manifiesto NLS (`package.nls.json` y `package.nls.es.json`).
- **Diagnósticos del servidor**: El servidor de lenguaje emite mensajes de diagnóstico en el idioma activo, garantizando que los errores de compilación y las descripciones en hover coincidan con tu preferencia.
- **Anulación manual de idioma**: Puedes seleccionar tu idioma explícitamente configurando el argumento de inicio del servidor o pasando `--locale=es` / `--locale=en`.

---

## Características principales

- **Diagnósticos semánticos**: Validación sintáctica y semántica en tiempo real con pasadas en segundo plano con control de rebote (debounce) e huella digital FNV-1a para evitar tormentas de análisis al guardar.
- **Comprobación de punteros nulos sensible al flujo**: Diagnósticos intraprocedimentales de desreferencia de identificadores nulos (`as-warn-possible-null-dereference`) que advierten sobre handles no comprobados o usados tras asignación a `null`.
- **Hover preciso**: Tooltips con aislamiento de sobrecargas en puntos de llamada, documentación Doxygen (`@brief`, `@param`, `@return`), resolución de constructores en inicialización directa, estado de archivos y rutas de activos en literales de texto, y contratos de lambdas anónimas.
- **Navegación e ir a definición**: Salto exacto a símbolos entre archivos y stubs con coincidencia de argumentos de sobrecarga (`FilterOverloadsForCall`), mapeo de origen de mixins y descubrimiento de implementaciones de interfaces (`Ctrl+F12`).
- **Autocompletado inteligente**: Sugerencias de miembros conscientes del ámbito (`.`), búsqueda en espacios de nombres (`::`), clasificación inteligente por compatibilidad de tipos en argumentos y asignaciones, y expansión de fragmentos de código.
- **Resaltado semántico**: Flujos delta de enteros con clasificación de tokens estándar de LSP que distingue parámetros, miembros, variables locales, tipos, constantes enum aisladas y ramas inactivas del preprocesador.
- **Inlay Hints**: Pistas en línea con nombres de parámetros, resaltado de rangos en argumentos de múltiples partes, valores omitidos de parámetros por defecto, tipos de respaldo para comodines y supresión configurable.
- **CodeLens y Jerarquía de llamadas**: Contadores de referencias entre archivos y espacios de nombres sobre declaraciones, rastreo de accesores virtuales y árbol bidireccional de llamadas (`textDocument/prepareCallHierarchy`).
- **Símbolos de documento y espacio de trabajo**: Esquemas jerárquicos de símbolos para migas de pan (breadcrumbs) y panel de esquema, más búsqueda difusa en todo el espacio de trabajo (`Ctrl+T`).
- **Documentos virtuales de mixins**: Inspección de documentos sintéticos (`angelscript-virtual://`) que permite visualización en línea y validación de miembros en el ámbito del host.

---

## Referencia de configuración

### Variables de ruta

Las configuraciones de rutas admiten expansión de variables dinámicas según el estándar `launch.json` de VS Code:

| Variable | Expansión |
| :--- | :--- |
| `${workspaceFolder}` | Directorio raíz de la carpeta de trabajo activa. |
| `${workspaceFolder:name}` | Directorio raíz de la carpeta de trabajo nombrada en un entorno multi-raíz. |
| `${userHome}` | Directorio del usuario actual. |
| `${env:NOMBRE}` | Valor de la variable de entorno `NOMBRE` (ej. `${env:SVENCOOP_DIR}`). |

### Catálogo de configuraciones

| Configuración | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `angelscript.searchDirectories` | `[]` | Directorios adicionales para la resolución de `#include "ruta.as"`. |
| `angelscript.include.implicitExtension` | `false` | Permite que `#include "helper"` resuelva a `helper.as` sin requerir la extensión. |
| `angelscript.predefinedFiles` | `[]` | Lista de archivos de stubs de API del host (`.as.predefined`). |
| `angelscript.predefined.active` | `""` | El stub activo a cargar cuando hay varios presentes. Usa `"all"` para fusionar todos. |
| `angelscript.predefinedExtension` | `.as.predefined` | Sufijo que identifica los archivos de stub en el espacio de trabajo. |
| `angelscript.modules` | `[]` | Definición de módulos de scripts especificados como `{"name", "entry"}` o `{"name", "folder"}`. |
| `angelscript.fileExtension` | `.as` | Extensión de los archivos de script analizados en el espacio de trabajo. |
| `angelscript.enableVirtualMixinDocuments` | `false` | Habilita proveedores de documentos virtuales para la expansión de clases mixin. |
| `angelscript.inlayHints.maxParameters` | `0` | Número máximo de pistas de parámetros a mostrar por llamada (`0` = sin límite). |
| `angelscript.inlayHints.maxLength` | `0` | Longitud máxima de caracteres en etiquetas de pistas de parámetros antes de truncar con `...` (`0` = sin límite). |
| `angelscript.inlayHints.omittedDefaultArguments` | `"off"` | Pistas para argumentos por defecto omitidos (`"nameAndValue"`, `"declaration"`, `"off"`). |
| `angelscript.hover.stringLiteralPathResolution` | `true` | Resuelve literales de cadena que parecen rutas de archivos/activos comprobando su existencia en disco. |
| `angelscript.completion.smartTypeRanking` | `true` | Clasificación contextual de tipos priorizando los que coinciden con el parámetro o asignación esperada. |
| `angelscript.statusBar.alignment` | `"left"` | Alineación del elemento de AngelScript en la barra de estado (`"left"` o `"right"`). |
| `angelscript.diagnosticSeverity` | `{}` | Anulación de severidad por código de diagnóstico (ej. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.engine.requireEnumScope` | `false` | Cuando está activo (`asEP_REQUIRE_ENUM_SCOPE`), los enums deben calificarse con `Enum::Miembro`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | Cuando está activo (`asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`), siempre se sintetiza un constructor por defecto. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | Cuando está activo (`asEP_IGNORE_DUPLICATE_SHARED_INTF`), se ignoran interfaces compartidas idénticas entre archivos. |
| `angelscript.features.*` | `true` | Conmutadores individuales para características LSP (hover, completion, formatting, etc.). |
| `angelscript.format.braceStyle` | `"allman"` | Estilo de llaves (`"allman"` o `"kr"`). |
| `angelscript.format.spacesInsideParentheses` | `false` | Si se deben insertar espacios dentro de los paréntesis (ej. `foo( bar )` en lugar de `foo(bar)`). |

Las modificaciones de configuración se aplican dinámicamente sin requerir recargar la ventana de VS Code.

---

## Confianza y seguridad del espacio de trabajo

AngelLSP implementa límites estrictos de seguridad bajo el modelo de confianza de espacio de trabajo de VS Code:
- En **Espacios de trabajo no confiables**, las rutas ejecutables de servidor personalizadas configuradas en el espacio de trabajo (`server.executablePath`) están estrictamente deshabilitadas e ignoradas.
- Solo se puede utilizar el ejecutable del servidor integrado o la configuración global del usuario, protegiendo contra ejecución remota de código mediante configuraciones maliciosas de repositorios.

---

## Agradecimientos y Créditos

- **Logo y Marca de AngelScript**: El icono oficial de AngelScript es una adaptación del sitio web de [AngelScript](https://www.angelcode.com/angelscript/) por Andreas Jönsson.
- **Iconos de archivo (`.as` / `.as.predefined`)**: Obtenidos del magnífico [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (específicamente el icono de ActionScript), tomados prestados temporalmente para pruebas mientras se diseñan los iconos personalizados de AngelScript—todo el crédito y agradecimiento a Philipp Kief y los contribuidores de Material Icon Theme :P.

---

## Licencia

Este proyecto está licenciado bajo la Licencia MIT. Consulta el archivo [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) para obtener más detalles.
