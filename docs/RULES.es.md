# Guía de Reglas del Motor en AngelScript (`angelscript.rules.json`)

[English](RULES.md) | [Español](RULES.es.md)

Si el motor llama a `MapInit` desde C++, el editor puede marcar la función como si nadie la usara, porque no ve ninguna llamada en los archivos `.as`. Agrégala a un archivo `*.angelscript.rules.json` para que el editor sepa que la llama el motor.

Ese mismo archivo te sirve para vincular `BaseClass.Spawn()` con la API del motor y señalar llamadas o declaraciones que tu proyecto restringe.

---

## Implementación de Referencia: Sven Co-op

En las reglas de Sven Co-op 5.26 puedes ver cómo se configuran los callbacks de entidades, las clases base nativas y los avisos para llamadas al scheduler.

👉 [`predefined/sven.angelscript.rules.json`](../predefined/sven.angelscript.rules.json)

---

## Cómo Habilitar las Reglas en tu Proyecto

### Método 1: Detección Automática (Cero Configuración)

Guarda `angelscript.rules.json` en la raíz del workspace. También puedes usar un nombre como `sven.angelscript.rules.json` o `motor.angelscript.rules.json`. El servidor busca primero el nombre exacto en la raíz, luego busca junto a los stubs y, por último, revisa si hay otros archivos `*.angelscript.rules.json` en la raíz.

### Método 2: Configuración en VS Code

Si quieres elegir el archivo, pon su ruta en `angelscript.rulesFile` dentro de `.vscode/settings.json`.

```jsonc
{
  "angelscript.rulesFile": "${workspaceFolder}/predefined/sven.angelscript.rules.json"
}
```

### Método 3: Paleta de Comandos

Abre la paleta con `Ctrl+Shift+P` (o `Cmd+Shift+P` en macOS) y ejecuta **AngelScript: Configurar reglas del motor y API predefinida...** (`angelscript.configureHostEnvironment`). Elige la opción para crear el archivo `angelscript.rules.json` inicial. VS Code abre la plantilla con una referencia `$schema` para que puedas editarla.

---

## Guía Práctica Sección por Sección

Parte de estos ejemplos y cambia los nombres por las funciones, los tipos y los callbacks que expone tu motor.

---

### 1. `engineProperties` — Opciones de Compilación y Sintaxis

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

Con `propertyAccessorMode` en `2`, puedes acceder a `obj.X` mediante `get_X()` o `set_X(v)`. Si `requireEnumScope` está en `false`, basta escribir `SELECTFIRE_SEMI` en vez de `INS2_SELECTFIRE_OPTIONS::SELECTFIRE_SEMI`. Usa los mismos valores `asEP_*` que en tu motor.

---

### 2. `unusedSymbolRules.ignoredGlobalFunctions` — Puntos de Entrada del Motor

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

Incluye `MapInit` y las demás funciones globales que llama el motor. El editor omite el aviso de función sin usar (`W118`) aunque ningún script las llame.

---

### 3. `unusedSymbolRules.ignoredBaseClasses` — Clases Base de Entidades del Motor

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

Si el motor crea `class weapon_galil : ScriptBasePlayerWeaponEntity`, agrega la clase base a la lista. El editor omite el aviso de clase sin usar (`W120`) en la clase derivada. El aviso para variables globales sin usar lleva el código `W117`.

---

### 4. `unusedSymbolRules.lifecycleMethods` — Métodos Universales del Ciclo de Vida

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

Agrega callbacks como `Spawn` y `Think` para quitarles el aviso de método sin usar (`W119`) en clases derivadas de las bases de `ignoredBaseClasses`.

Si dejas vacía la lista `ignoredBaseClasses`, la exención se aplica a los métodos que coincidan en cualquier clase.

Si el stub `.as.predefined` activo y `baseClassProxyMap` ya permiten vincular un callback con un método del motor, puedes dejarlo fuera de la lista.

---

### 5. `unusedSymbolRules.baseClassLifecycleMethods` — Métodos Específicos por Tipo de Entidad

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

En este ejemplo, `PrimaryAttack` y `Reload` quedan exentos en clases que heredan de `ScriptBasePlayerWeaponEntity`. Si otra clase tiene un método `PrimaryAttack` que nadie usa, el editor sigue marcándolo.

Si `baseClassProxyMap` y tus stubs ya identifican un método del motor, puedes omitirlo aquí.

---

### 6. `unusedSymbolRules.baseClassProxyMap` y `baseClassDelegateProperties` — Vinculación con Clases Nativas C++

#### Ejemplo

```json
{
  "unusedSymbolRules": {
    "baseClassDelegateProperties": [
      "BaseClass"
    ],
    "baseClassProxyMap": {
      "ScriptBaseEntity": ["BaseEntity", "CBaseEntity"],
      "ScriptBaseMonsterEntity": ["BaseMonster", "CBaseMonster"],
      "ScriptBasePlayerWeaponEntity": ["BasePlayerWeapon", "CBasePlayerWeapon"]
    }
  }
}
```

En Sven Co-op, `BaseClass` es una propiedad delegada. Asocia la base de script `ScriptBasePlayerWeaponEntity` con `BasePlayerWeapon` o `CBasePlayerWeapon` en el stub `.as.predefined`. Si está disponible la declaración nativa, puedes abrirla con Ir a definición (`Ctrl+Click`) sobre `BaseClass.PrimaryAttack()` y ver información del stub en el hover.

---

### 7. `unusedSymbolRules.stringReflectionCallees` — Registro por Reflexión y Cadenas

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

Si escribes `g_CustomEntityFuncs.RegisterCustomEntity("weapon_galil", "weapon_galil")`, `argIndices` indica que la cadena en la posición `0` cuenta como un uso de `weapon_galil`. Así el editor puede reconocer que la clase se usa aunque el script no la instancie.

---

### 8. `storageRules` — Seguridad de Memoria y Manejo de Handles

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

La regla del ejemplo puede marcar `private CBasePlayer@ m_pTarget;` si el tipo coincide con el filtro de entidades. El aviso se aplica a miembros y arreglos y sugiere guardar un `EHandle` en lugar del handle directo.

---

### 9. `callRules` (anteriormente `schedulerRules`) — Seguridad en Llamadas y Tareas Asíncronas

#### Ejemplo

```json
{
  "callRules": [
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

Si pasas un handle de entidad que coincide con la regla a `g_Scheduler.SetTimeout(...)` o `g_Scheduler.SetInterval(...)`, el editor marca ese argumento (`W114`). El mensaje del ejemplo sugiere usar `EHandle` para los argumentos que usará el callback más adelante.

Los archivos antiguos pueden usar `schedulerRules` como alias de `callRules`.

---

### 10. `noHandleTypes` — Prohibición de Handles en Tipos de Valor del Motor

#### Ejemplo

```json
{
  "noHandleTypes": ["string", "datetime", "complex"]
}
```

Con esta lista, `string@ s;` y `array<string@> items;` dan `as-err-handle-on-no-handle-type` (`E261`). La regla también revisa los alias de `typedef`. En `string[]@` o `array<string>@`, el handle se aplica al arreglo.

También puedes indicar los tipos en `angelscript.engine.noHandleTypes` o con `--no-handle-type=<name>` al iniciar el servidor. El ajuste y el archivo de reglas se combinan sin duplicar tipos.

---

## Validación por Esquema y Autocompletado

Agrega `$schema` a tu archivo `*.angelscript.rules.json` para que VS Code revise los campos y te sugiera nombres de propiedades.

```json
{
  "$schema": "https://raw.githubusercontent.com/Gaftherman/angelscript-lsp/main/schemas/angelscript.rules.schema.json"
}
```

Si guardas el archivo en una carpeta al mismo nivel que `schemas`, puedes usar una ruta relativa.

```json
{
  "$schema": "../schemas/angelscript.rules.schema.json"
}
```
