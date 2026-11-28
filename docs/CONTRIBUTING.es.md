# Contribuir a AngelLSP

**[English](../CONTRIBUTING.md)** | **[Español](CONTRIBUTING.es.md)**

Gracias por ayudar. Se agradecen los informes de errores, las correcciones, la documentación y los ejemplos.

## Informar de un problema

Abre una [incidencia](https://github.com/angelscript-tools/angelscript-lsp/issues) con la versión que usas, tu sistema operativo, los pasos para reproducirlo y la salida que viste. Lo que más ayuda es un script de pocas líneas que reproduzca el problema.

## Proponer un cambio

1. Haz un fork del repositorio y crea una rama desde `main`.
2. Mantén cada cambio pequeño y enfocado, con pruebas cuando cambie el comportamiento.
3. Escribe los mensajes de commit como `tipo(ámbito): descripción breve en imperativo` sin punto final. Tipos: `feat`, `fix`, `test`, `perf`, `refactor`, `style`, `docs`, `chore`.
4. Abre una pull request que explique qué cambió y cómo lo comprobaste.

## Compilar y probar

El manual de ingeniería en [AGENTS.md](AGENTS.md) define la arquitectura, las reglas y las verificaciones. Compila el servidor y ejecuta las comprobaciones antes de enviar un cambio:

~~~sh
cmake -S server -B server/build -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
python server/scripts/check-quality.py
~~~

El cliente está en `client/` (`npm ci`, `npm run bundle`).

## Estilo

Sigue `AGENTS.md` (§§3–5, §8). Da formato a C++ con `python server/scripts/format-code.py`. Los mensajes del servidor van en `server/src/i18n/locales/en.json` y `es.json`.

## Licencia

Al contribuir aceptas que tu trabajo se publique bajo la [licencia MIT](../LICENSE).

## Proyectos relacionados

| Proyecto | Uso |
| --- | --- |
| [angelscript-dap](https://github.com/angelscript-tools/angelscript-dap) | El núcleo del depurador |
| [angelscript-debugger-example](https://github.com/angelscript-tools/angelscript-debugger-example) | Una aplicación que muestra cómo usar el depurador |
| [angelscript-cli](https://github.com/angelscript-tools/angelscript-cli) | Ejecuta, comprueba y depura scripts desde la terminal |
| [angelscript-lsp](https://github.com/angelscript-tools/angelscript-lsp) | Soporte del editor para VS Code |
