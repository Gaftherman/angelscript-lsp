# Angelscript para VS Code

**[English](../README.md)** | **[Español](README.es.md)**

Angelscript agrega autocompletado, documentación al pasar el cursor, detección de errores y navegación de código para archivos AngelScript (`.as`) en VS Code.

![Documentación al pasar el cursor sobre una llamada](../client/images/hover.gif)
*Deja el cursor sobre una función o método para ver su firma y su comentario de documentación.*

## Primeros pasos

1. Instala la extensión desde [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman).
2. Abre una carpeta que contenga archivos de script `.as`.
3. Opcional: agrega un archivo `.as.predefined` a tu carpeta si tu juego ofrece tipos o funciones propias.

## Qué obtienes

| Función | Qué ves | Imagen |
| :--- | :--- | :--- |
| Hover | Observa cómo aparece la firma y la documentación al dejar el cursor sobre un símbolo. | ![Documentación hover](../client/images/hover.gif) |
| Autocompletado | Observa cómo aparecen los métodos y campos disponibles al escribir después de `.` o `::`. | ![Autocompletado de miembros](../client/images/completion.gif) |
| Diagnósticos | Observa los subrayados rojos y amarillos en errores de escritura y variables sin usar mientras escribes. | ![Diagnósticos al escribir](../client/images/diagnostics.gif) |
| Ayuda de firma | Los nombres y tipos de parámetros siguen visibles mientras completas una llamada. | ![Ayuda de firma](../client/images/signature-help.png) |
| Esquema | El panel Esquema muestra cada espacio de nombres, clase, campo y método del archivo. | ![Panel de esquema](../client/images/outline.png) |

## Indica qué ofrece tu juego

Tu motor expone clases, funciones y variables globales que no están escritas dentro de tus scripts `.as`. Puedes declararlas en un archivo `.as.predefined`, que es un archivo que lista lo que el juego ofrece a los scripts (conocido como stub). Con ese archivo en `${workspaceFolder}`, el autocompletado y el hover reconocen la API del juego sin falsos errores.

![API predefinida del juego en autocompletado](../client/images/predefined.gif)
*Observa cómo el editor reconoce los tipos y métodos del juego en cuanto se carga el archivo `.as.predefined`.*

![Ejemplo de archivo .as.predefined](../client/images/predefined-file.png)

## Agrega tus propias reglas

Un archivo `*.angelscript.rules.json` permite a tu equipo fijar opciones del motor y añadir sus propios avisos. Por ejemplo, avisar cuando un tipo se guarda en un campo de clase. Pon el archivo en `${workspaceFolder}` y el editor lo carga solo.

![Reglas personalizadas del motor en acción](../client/images/rules.gif)
*Observa cómo una regla personalizada marca un campo no permitido dentro del editor.*

![Ejemplo de archivo .angelscript.rules.json](../client/images/rules-file.png)

## Ajusta el comportamiento a tu motor

Cada juego configura AngelScript con sus propias reglas de sintaxis, como permitir bucles `foreach` o cadenas en varias líneas. Puedes igualar esa configuración desde los ajustes `angelscript.engine.*` en VS Code. Al cambiar una opción, las comprobaciones del editor se actualizan de inmediato.

![Opción del motor cambiando las comprobaciones del editor](../client/images/engine-options.gif)
*Observa cómo aparece o desaparece un error al cambiar una opción del motor en la configuración.*

## Documentación

- [Guía detallada](guide.es.md) — ejemplos de espacio de trabajo, archivos predefinidos, opciones del motor, comandos y catálogo completo de ajustes.
- [Guía de reglas del motor](rules.es.md) — cómo escribir archivos `*.angelscript.rules.json`.
- [CHANGELOG.md](../CHANGELOG.md) — historial de versiones y cambios.

## Compilar desde el código fuente

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile
```

Bajo licencia [MIT](../LICENSE).
