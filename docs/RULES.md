# AngelScript Engine Rules Guide (`angelscript.rules.json`)

[English](RULES.md) | [Español](RULES.es.md)

If your game calls `MapInit` from C++, the editor may mark it as unused because there’s no call in the `.as` files. Add it to a `*.angelscript.rules.json` file to tell the editor how the host uses your scripts.

The same file can point `BaseClass.Spawn()` to the host API and flag calls or declarations your project restricts.

---

## Reference Implementation: Sven Co-op

Use the Sven Co-op 5.26 rules to see how entity callbacks, native base classes, and scheduler checks fit together.

👉 [`predefined/sven.angelscript.rules.json`](../predefined/sven.angelscript.rules.json)

---

## How to Enable Rules in Your Project

### Method 1: Automatic Discovery (Zero Configuration)

Put `angelscript.rules.json` at the workspace root. You can also use a name such as `sven.angelscript.rules.json` or `engine.angelscript.rules.json`. For automatic discovery, the server checks the exact root filename first, then looks next to stubs, then checks the root for other `*.angelscript.rules.json` files.

### Method 2: VS Code Settings

To choose a specific rules file, set `angelscript.rulesFile` in `.vscode/settings.json`.

```jsonc
{
  "angelscript.rulesFile": "${workspaceFolder}/predefined/sven.angelscript.rules.json"
}
```

### Method 3: Command Palette

Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on macOS) and run **AngelScript: Configure Host Rules & Predefined API...** (`angelscript.configureHostEnvironment`). Choose **Create Starter angelscript.rules.json in Workspace** to open an editable template with a `$schema` reference.

---

## Practical Section-by-Section Guide

Use the examples below to match the rules to the functions, types, and callbacks your host exposes.

---

### 1. `engineProperties` — Compiler & Syntax Options

#### Example

```json
{
  "engineProperties": {
    "propertyAccessorMode": 2,
    "requireEnumScope": false,
    "disallowGlobalVars": false
  }
}
```

With `propertyAccessorMode` set to `2`, `get_X()` and `set_X(v)` can expose `obj.X`. With `requireEnumScope` set to `false`, the editor accepts `SELECTFIRE_SEMI` without `INS2_SELECTFIRE_OPTIONS::SELECTFIRE_SEMI`. Set these `asEP_*` options to match your host.

---

### 2. `unusedSymbolRules.ignoredGlobalFunctions` — Engine Entry Points

#### Example

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

Add `MapInit` and other global functions the host calls directly. The editor skips the unused-function warning (`W118`) for these functions even when your scripts never call them.

---

### 3. `unusedSymbolRules.ignoredBaseClasses` — Engine Entity Base Classes

#### Example

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

If `class weapon_galil : ScriptBasePlayerWeaponEntity` is created by the engine, list its base class. That exempts the derived class from the unused-class warning (`W120`). The unused-global-variable warning uses `W117`.

---

### 4. `unusedSymbolRules.lifecycleMethods` — Universal Entity Callbacks

#### Example

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

Add callbacks such as `Spawn` and `Think` to exempt them from the unused-method warning (`W119`) on classes derived from the bases in `ignoredBaseClasses`.

With an empty `ignoredBaseClasses` list, the exemption applies to matching methods on any class.

When the active `.as.predefined` stub and `baseClassProxyMap` already let the editor match a script callback to a host method, you can leave that callback out of this list.

---

### 5. `unusedSymbolRules.baseClassLifecycleMethods` — Entity-Specific Callbacks

#### Example

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

Use this mapping to exempt `PrimaryAttack` and `Reload` on classes derived from `ScriptBasePlayerWeaponEntity`. An unrelated class with an unused method named `PrimaryAttack` still gets a warning.

If `baseClassProxyMap` and your stubs already identify a host method, you can omit it here.

---

### 6. `unusedSymbolRules.baseClassProxyMap` & `baseClassDelegateProperties` — Native Engine Base Class Linking

#### Example

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

In the Sven Co-op example, `BaseClass` is a delegate property. Map its script base `ScriptBasePlayerWeaponEntity` to `BasePlayerWeapon` or `CBasePlayerWeapon` in the `.as.predefined` stub. When the native declaration is available, Go to Definition (`Ctrl+Click`) on `BaseClass.PrimaryAttack()` can open it, and hover can show information from the stub.

---

### 7. `unusedSymbolRules.stringReflectionCallees` — Reflection & String Symbol References

#### Example

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

For `g_CustomEntityFuncs.RegisterCustomEntity("weapon_galil", "weapon_galil")`, `argIndices` tells the editor to count the string at index `0` as a use of `weapon_galil`. The class can then count as used even when the scripts never instantiate it directly.

---

### 8. `storageRules` — Memory Safety & Handle Governance

#### Example

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
      "message": "Entity handle '{}' in member or array can dangle if removed by engine; store 'EHandle' instead.",
      "severity": "warning"
    }
  ]
}
```

With this rule, `private CBasePlayer@ m_pTarget;` can show a warning when the type matches the entity rule. The example flags member and array storage and asks you to use `EHandle` in place of a matching raw entity handle.

---

### 9. `callRules` (formerly `schedulerRules`) — Call & Asynchronous Safety

#### Example

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
      "message": "Passing raw entity handle to scheduled callback is unsafe. Use 'EHandle'.",
      "severity": "warning"
    }
  ]
}
```

When you pass a matching entity handle to `g_Scheduler.SetTimeout(...)` or `g_Scheduler.SetInterval(...)`, the editor flags the argument (`W114`). This example’s message suggests `EHandle` for arguments the callback will use later.

Older files can use `schedulerRules` as an alias for `callRules`.

---

### 10. `noHandleTypes` — Disallowing Handles on Engine Value Types

#### Example

```json
{
  "noHandleTypes": ["string", "datetime", "complex"]
}
```

With this list, `string@ s;` and `array<string@> items;` report `as-err-handle-on-no-handle-type` (`E261`). The check also applies through `typedef` aliases. In `string[]@` or `array<string>@`, the handle applies to the array.

You can also list the types in `angelscript.engine.noHandleTypes` or pass `--no-handle-type=<name>` to the server. Entries from the setting and rules file are combined without duplicates.

---

## Schema Validation & IntelliSense

Add `$schema` to your `*.angelscript.rules.json` file so VS Code can check the rule fields and suggest property names.

```json
{
  "$schema": "https://raw.githubusercontent.com/Gaftherman/angelscript-lsp/main/schemas/angelscript.rules.schema.json"
}
```

If the rules file is in a directory next to `schemas`, you can use a relative path.

```json
{
  "$schema": "../schemas/angelscript.rules.schema.json"
}
```
