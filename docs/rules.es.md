# Guía de Reglas del Motor en AngelScript (`angelscript.rules.json`)

**[English](rules.md)** | **[Español](rules.es.md)**

AngelLSP es completamente agnóstico al motor: por defecto, aplica la semántica estándar de AngelScript puro sin ninguna función de retorno o callback de juego cableada en su código C++.

Sin embargo, en motores reales y aplicaciones anfitrionas (como [Sven Co-op](https://www.svencoop.com/), motores de videojuegos personalizados o integraciones con `CScriptBuilder`), el motor C++ invoca funciones de script, instancia clases de entidades y accede a propiedades de manera dinámica mediante reflexión o ganchos del host sin que existan llamadas explícitas en los scripts `.as`.

Para evitar falsos positivos de símbolos "sin usar", resolver con precisión llamadas del motor como `BaseClass.Spawn()` y aplicar directrices de seguridad del host, AngelLSP utiliza **archivos de Reglas del Motor** (`*.angelscript.rules.json`).

---

## Implementación de Referencia: Sven Co-op

Para un ejemplo completo y listo para producción configurado para Sven Co-op 5.26+ (incluyendo clases base de entidades, métodos de ciclo de vida, seguridad del programador de tareas y mapas de proxies), consulta:

👉 **[`predefined/sven.angelscript.rules.json`](../predefined/sven.angelscript.rules.json)**

---

## Cómo Habilitar las Reglas en tu Proyecto

Puedes activar un archivo de reglas en tu espacio de trabajo mediante tres métodos sencillos:

### Método 1: Detección Automática (Cero Configuración)
Simplemente coloca un archivo con terminación `.angelscript.rules.json` (por ejemplo, `sven.angelscript.rules.json` o `motor.angelscript.rules.json`) en la raíz de tu proyecto. AngelLSP lo detectará y cargará automáticamente.

### Método 2: Configuración en VS Code
Especifica la ruta explícita en `.vscode/settings.json`:
```jsonc
{
  "angelscript.rulesFile": "${workspaceFolder}/predefined/sven.angelscript.rules.json"
}
```

### Método 3: Paleta de Comandos
Presiona `Ctrl+Shift+P` (o `Cmd+Shift+P` en macOS) y ejecuta:
`AngelScript: Configurar Reglas del Motor Host` (`angelscript.configureHostEnvironment`).
Este comando generará una plantilla de reglas validada por esquema adaptada a tu espacio de trabajo.

---

## Guía Práctica Sección por Sección

El archivo de reglas está estructurado en varias secciones. A continuación se explica para qué sirve cada una, un ejemplo JSON conciso y el comportamiento resultante obtenido en tu editor.

---

### 1. `engineProperties` — Opciones de Compilación y Sintaxis

Configura las propiedades nativas del motor AngelScript (`asEP_*`) coincidiendo con la configuración que tu aplicación host C++ aplicó al motor de scripts.

#### Ejemplo
```json
{
  "engineProperties": {
    "propertyAccessorMode": 2,
    "requireEnumScope": false,
    "disallowGlobalVars": false
  }
}
```

* **Para qué se usa**:
  * `propertyAccessorMode`: `2` habilita propiedades virtuales automáticas (accesores `get_` y `set_` accesibles directamente mediante `objeto.Propiedad`).
  * `requireEnumScope`: Con `false`, las constantes de enumeración pueden usarse directamente (`SELECTFIRE_SEMI`) sin requerir el tipo del enum (`INS2_SELECTFIRE_OPTIONS::SELECTFIRE_SEMI`).
* **Resultado**: El autocompletado y el análisis semántico permiten valores de enumeración directos y accesores virtuales sin emitir diagnósticos de error.

---

### 2. `unusedSymbolRules.ignoredGlobalFunctions` — Puntos de Entrada del Motor

Lista las funciones globales invocadas directamente por el motor C++ (como rutinas de inicialización, eventos de inicio de mapa o comandos de consola).

#### Ejemplo
```json
{
  "unusedSymbolRules": {
    "ignoredGlobalFunctions": [
      "PluginInit",
      "PluginExit",
      "MapInit",
      "MapStart",
      "MapActivate"
    ]
  }
}
```

* **Para qué se usa**: Indica al servidor de lenguaje que estas funciones globales son puntos de entrada invocados externamente por el motor.
* **Resultado**: AngelLSP **no** mostrará la advertencia `W114` ("La función 'MapInit' no se utiliza nunca"), incluso si tiene 0 llamadas dentro de tus scripts.

---

### 3. `unusedSymbolRules.ignoredBaseClasses` — Clases Base de Entidades del Motor

Lista las clases base o interfaces del motor. Las clases en tus scripts que hereden de cualquiera de estas clases representan entidades del juego creadas por el motor o colocadas en un editor de mapas (ej. Hammer).

#### Ejemplo
```json
{
  "unusedSymbolRules": {
    "ignoredBaseClasses": [
      "ScriptBaseEntity",
      "ScriptBaseMonsterEntity",
      "ScriptBasePlayerWeaponEntity",
      "ScriptClassInterface"
    ]
  }
}
```

* **Para qué se usa**: Las clases que extienden estos tipos (ej. `class weapon_galil : ScriptBasePlayerWeaponEntity`) se reconocen automáticamente como entidades del juego.
* **Resultado**: La definición de la clase **no** se marcará como clase sin usar (`W117`), y sus métodos de ciclo de vida serán protegidos.

---

### 4. `unusedSymbolRules.lifecycleMethods` — Métodos Universales del Ciclo de Vida

Métodos que el motor invoca dinámicamente en cualquier clase de entidad cuando ocurren eventos en el juego.

#### Ejemplo
```json
{
  "unusedSymbolRules": {
    "lifecycleMethods": [
      "Spawn",
      "Precache",
      "Think",
      "Touch",
      "Use",
      "Blocked",
      "Killed"
    ]
  }
}
```

* **Para qué se usa**: Exime a estos métodos de la comprobación de métodos sin usar cuando se declaran dentro de una clase de entidad derivada de `ignoredBaseClasses`.
* **Resultado**: Escribir `void Spawn() { ... }` o `void Think() { ... }` en tu clase de monstruo o arma personalizada nunca generará la advertencia "El método 'Spawn' no se utiliza nunca" (`W118`).

---

### 5. `unusedSymbolRules.baseClassLifecycleMethods` — Métodos Específicos por Tipo de Entidad

Define métodos que solo son callbacks válidos en tipos de entidad específicos (por ejemplo, métodos exclusivos de armas frente a métodos exclusivos de monstruos).

#### Ejemplo
```json
{
  "unusedSymbolRules": {
    "baseClassLifecycleMethods": {
      "ScriptBasePlayerWeaponEntity": [
        "PrimaryAttack",
        "SecondaryAttack",
        "Reload",
        "WeaponIdle",
        "Deploy",
        "Holster"
      ]
    }
  }
}
```

* **Para qué se usa**: Solo las clases que heredan de `ScriptBasePlayerWeaponEntity` considerarán `PrimaryAttack` y `Reload` como callbacks del motor. Una clase común y corriente que declare `PrimaryAttack` seguirá siendo alertada si no se usa.
* **Resultado**: Los scripts de armas se mantienen limpios de falsas advertencias, mientras que el código muerto accidental en clases normales se sigue detectando con total fidelidad.

---

### 6. `unusedSymbolRules.baseClassProxyMap` y `baseClassDelegateProperties` — Vinculación con Clases Nativas C++

En muchos entornos de script (como `ScriptBasePlayerWeaponEntity` en Sven Co-op), las clases de entidad de script no heredan directamente de la clase C++ nativa. En su lugar, exponen una propiedad delegada (como `BaseClass` o `self`) que apunta a la clase nativa (como `BasePlayerWeapon` o `CBasePlayerWeapon`).

#### Ejemplo
```json
{
  "unusedSymbolRules": {
    "baseClassDelegateProperties": [
      "BaseClass",
      "self"
    ],
    "baseClassProxyMap": {
      "ScriptBaseEntity": ["BaseEntity", "CBaseEntity"],
      "ScriptBaseMonsterEntity": ["BaseMonster", "CBaseMonster"],
      "ScriptBasePlayerWeaponEntity": ["BasePlayerWeapon", "CBasePlayerWeapon"]
    }
  }
}
```

* **Para qué se usa**: Conecta las llamadas `BaseClass.Metodo()` dentro de tu clase de script con los métodos correspondientes de la clase nativa C++ declarados en el stub `.as.predefined`.
* **Resultado**:
  * ¡Hacer `Ctrl+Click` (Ir a Definición) en `BaseClass.Spawn()` o `BaseClass.PrimaryAttack()` te lleva directamente a `BasePlayerWeapon::PrimaryAttack` en tu stub activo `sven.as.predefined`!
  * Al pasar el cursor por encima de `BaseClass`, se muestra la documentación y firmas de la clase nativa del motor.
  * El conteo de referencias hacia los métodos de `BaseClass` se calcula con absoluta precisión.

---

### 7. `unusedSymbolRules.stringReflectionCallees` — Registro por Reflexión y Cadenas

Registra funciones del motor que reciben nombres de entidades o funciones de callback mediante cadenas de texto literales.

#### Ejemplo
```json
{
  "unusedSymbolRules": {
    "stringReflectionCallees": [
      {
        "callee": "g_CustomEntityFuncs",
        "methods": ["RegisterCustomEntity"],
        "argIndices": [0]
      },
      {
        "callee": "g_Scheduler",
        "methods": ["SetTimeout", "SetInterval"],
        "argIndices": [0]
      }
    ]
  }
}
```

* **Para qué se usa**: Al escribir `g_CustomEntityFuncs.RegisterCustomEntity("weapon_galil", "weapon_galil")`, la cadena `"weapon_galil"` en el argumento `0` se computa como una referencia activa a `class weapon_galil`.
* **Resultado**:
  * La clase `weapon_galil` se marca como utilizada al registrarse mediante reflexión.
  * ¡Hacer `Ctrl+Click` en `"weapon_galil"` dentro de la llamada de registro salta directamente a la declaración de la clase!

---

### 8. `storageRules` — Seguridad de Memoria y Manejo de Handles

Detecta y previene patrones peligrosos de almacenamiento que provocan fugas de memoria, punteros colgantes o caídas del servidor.

#### Ejemplo
```json
{
  "storageRules": [
    {
      "id": "unsafe-entity-handle",
      "inheritsFrom": "CBaseEntity",
      "typeRegex": "CBase.*",
      "handleOnly": true,
      "disallowInMembers": true,
      "disallowInArrays": true,
      "suggestReplacement": "EHandle",
      "message": "El handle de entidad '{}' en miembro o arreglo puede quedar colgando si la entidad es eliminada; almacena 'EHandle' en su lugar.",
      "severity": "warning"
    }
  ]
}
```

* **Para qué se usa**: En motores GoldSource / Half-Life, almacenar un handle directo `CBaseEntity@` en variables miembro o arreglos es peligroso: si la entidad muere o desaparece, el handle queda colgando y causa un crash del servidor. La regla detecta esto y sugiere `EHandle`.
* **Resultado**: Escribir `private CBasePlayer@ m_pTarget;` en una clase genera de inmediato una advertencia diagnóstica en VS Code recomendando usar `EHandle m_hTarget;`.

---

### 9. `schedulerRules` — Seguridad en Tareas Asíncronas

Valida las invocaciones al programador de tareas para asegurar que los argumentos pasados sigan siendo válidos cuando se ejecute la tarea retardada.

#### Ejemplo
```json
{
  "schedulerRules": [
    {
      "id": "scheduler-safety",
      "callee": "g_Scheduler",
      "methods": ["SetTimeout", "SetInterval"],
      "disallowArgInheritsFrom": "CBaseEntity",
      "disallowArgRegex": "CBase.*",
      "disallowArgHandleOnly": true,
      "argSuggestReplacement": "EHandle",
      "requireExplicitEnumConstruct": true,
      "message": "Pasar un handle crudo de entidad al callback programado es inseguro. Utiliza 'EHandle'.",
      "severity": "warning"
    }
  ]
}
```

* **Para qué se usa**: Alerta sobre handles de entidad pasados a `g_Scheduler.SetTimeout(...)`, evitando que apunten a memoria liberada si la entidad se destruye antes de dispararse el temporizador.
* **Resultado**: Protege a los desarrolladores de bloqueos y caídas sutiles del servidor antes de probar en el juego.

---

## Validación por Esquema y Autocompletado

Todos los archivos `*.angelscript.rules.json` cuentan con validación completa y autocompletado de esquema JSON en VS Code. Añade el encabezado `$schema` al inicio de tu archivo:

```json
{
  "$schema": "https://raw.githubusercontent.com/Gaftherman/angelscript-lsp/main/schemas/angelscript.rules.schema.json"
}
```
O utiliza la ruta relativa si trabajas dentro del repositorio:
```json
{
  "$schema": "../schemas/angelscript.rules.schema.json"
}
```

¡Con el esquema configurado, VS Code mostrará descripciones emergentes y sugerencias inteligentes para cada propiedad de regla!
