# AngelScript Engine Rules Guide (`angelscript.rules.json`)

**[English](rules.md)** | **[Español](rules.es.md)**

AngelLSP is completely engine-agnostic: by default, it enforces standard vanilla AngelScript semantics with zero hardcoded game or engine callbacks in C++. 

However, in real-world game engines and host applications (such as [Sven Co-op](https://www.svencoop.com/), custom game engines, or `CScriptBuilder` integrations), the C++ engine frequently invokes script functions, instantiates entity classes, and accesses properties dynamically via reflection or host hooks without explicit call sites in the `.as` script files.

To prevent false-positive "unused" warnings, accurately resolve engine wrapper calls like `BaseClass.Spawn()`, and enforce host safety rules, AngelLSP uses **Engine Rules files** (`*.angelscript.rules.json`).

---

## Reference Implementation: Sven Co-op

For a complete, real-world example configured for Sven Co-op 5.26+ (including entity base classes, lifecycle callbacks, scheduler safety, and proxy maps), see:

👉 **[`predefined/sven.angelscript.rules.json`](../predefined/sven.angelscript.rules.json)**

---

## How to Enable Rules in Your Project

You can activate an engine rules file in your workspace through three easy methods:

### Method 1: Automatic Discovery (Zero Configuration)
Simply place a file ending with `.angelscript.rules.json` (for example, `sven.angelscript.rules.json` or `engine.angelscript.rules.json`) in the root of your workspace. AngelLSP detects and loads it automatically.

### Method 2: VS Code Settings
Explicitly set the path in `.vscode/settings.json`:
```jsonc
{
  "angelscript.rulesFile": "${workspaceFolder}/predefined/sven.angelscript.rules.json"
}
```

### Method 3: Command Palette
Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on macOS) and run:
`AngelScript: Configure Host Engine Rules` (`angelscript.configureHostEnvironment`).
This command generates a schema-validated rules template tailored to your workspace.

---

## Practical Section-by-Section Guide

The rules file is structured into several sections. Below is an explanation of each section, what it does, a concise JSON example, and the resulting behavior in your editor.

---

### 1. `engineProperties` — Compiler & Syntax Options

Configures native AngelScript engine property flags (`asEP_*`) matching how your host C++ application configured the script engine.

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

* **What it does**:
  * `propertyAccessorMode`: `2` enables automatic virtual properties (`get_` and `set_` accessors accessible via `obj.Prop`).
  * `requireEnumScope`: When `false`, enum constants can be used directly (`SELECTFIRE_SEMI`) without qualifying them with the enum type (`INS2_SELECTFIRE_OPTIONS::SELECTFIRE_SEMI`).
* **Resulting Behavior**: Autocompletion and syntax analysis allow unqualified enum values and virtual property getters/setters without emitting diagnostic errors.

---

### 2. `unusedSymbolRules.ignoredGlobalFunctions` — Engine Entry Points

Lists global functions called directly by the engine from C++ (such as initialization routines, map start events, or command callbacks).

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

* **What it does**: Tells the language server that these global functions are entry points called by the engine.
* **Resulting Behavior**: AngelLSP will **not** show warning `W114` ("Function 'MapInit' is never used"), even if there are 0 calls to it in your script code.

---

### 3. `unusedSymbolRules.ignoredBaseClasses` — Engine Entity Base Classes

Lists entity base classes or interfaces. Classes in your scripts that inherit from any of these base classes represent game entities created dynamically by the engine or placed in a map editor (e.g. Hammer).

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

* **What it does**: Classes extending these types (e.g. `class weapon_galil : ScriptBasePlayerWeaponEntity`) are recognized as engine entities.
* **Resulting Behavior**: The class definition is **not** flagged as an unused class (`W117`), and its lifecycle callbacks are monitored.

---

### 4. `unusedSymbolRules.lifecycleMethods` — Universal Entity Callbacks

Methods that the engine calls dynamically on any entity class when events occur.

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

* **What it does**: Exempts these methods from unused method checks whenever they appear inside an entity class derived from `ignoredBaseClasses`.
* **Resulting Behavior**: Writing `void Spawn() { ... }` or `void Think() { ... }` in your custom monster or weapon class will never trigger "Method 'Spawn' is never used" (`W118`).

---

### 5. `unusedSymbolRules.baseClassLifecycleMethods` — Entity-Specific Callbacks

Defines lifecycle methods that are only valid callbacks on specific entity types (for example, weapon-only callbacks versus monster-only callbacks).

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

* **What it does**: Only classes inheriting from `ScriptBasePlayerWeaponEntity` treat `PrimaryAttack` and `Reload` as engine callbacks. A generic class declaring `PrimaryAttack` will still be warned if unused.
* **Resulting Behavior**: Weapon scripts stay clean without warnings, while accidental dead code in ordinary utility classes is still accurately caught.

---

### 6. `unusedSymbolRules.baseClassProxyMap` & `baseClassDelegateProperties` — Native Engine Base Class Linking

In many script frameworks (like Sven Co-op's `ScriptBasePlayerWeaponEntity`), script entity classes do not inherit directly from the native C++ engine class. Instead, they expose a delegate property (such as `BaseClass` or `self`) pointing to the native class (like `BasePlayerWeapon` or `CBasePlayerWeapon`).

#### Example
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

* **What it does**: Connects calls to `BaseClass.Method()` inside your script class to the corresponding methods in the native C++ class declared in `.as.predefined`.
* **Resulting Behavior**:
  * `Ctrl+Click` (Go to Definition) on `BaseClass.Spawn()` or `BaseClass.PrimaryAttack()` takes you directly to `BasePlayerWeapon::PrimaryAttack` in your active `sven.as.predefined` stub!
  * Hovering over `BaseClass` displays full documentation and signatures from the native engine class.
  * References to `BaseClass` methods are counted accurately.

---

### 7. `unusedSymbolRules.stringReflectionCallees` — Reflection & String Symbol References

Registers engine functions that take entity or callback names as string literals.

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

* **What it does**: When you write `g_CustomEntityFuncs.RegisterCustomEntity("weapon_galil", "weapon_galil")`, the string `"weapon_galil"` at argument index `0` is treated as a reference to `class weapon_galil`.
* **Resulting Behavior**:
  * `class weapon_galil` is marked as used because it was registered via string reflection.
  * `Ctrl+Click` on `"weapon_galil"` inside the registration call jumps straight to the class declaration!

---

### 8. `storageRules` — Memory Safety & Handle Governance

Detects and prevents unsafe storage patterns that lead to memory leaks, use-after-free, or server crashes.

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

* **What it does**: In GoldSource / Half-Life engines, storing a raw `CBaseEntity@` handle in a member variable or array is unsafe because if the entity is killed, the handle becomes dangling and crashes the server. The rule flags this and suggests `EHandle`.
* **Resulting Behavior**: Writing `private CBasePlayer@ m_pTarget;` inside a class immediately generates a warning diagnostic in VS Code recommending `EHandle m_hTarget;`.

---

### 9. `schedulerRules` — Asynchronous Callback Safety

Validates scheduler registrations to ensure arguments passed to delayed tasks remain valid when executed.

#### Example
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
      "message": "Passing raw entity handle to scheduled callback is unsafe. Use 'EHandle'.",
      "severity": "warning"
    }
  ]
}
```

* **What it does**: Flags raw entity handle arguments passed to `g_Scheduler.SetTimeout(...)`, ensuring they don't dangle if the entity is destroyed before the timer fires.
* **Resulting Behavior**: Protects developers from subtle timing crashes before testing in-game.

---

## Schema Validation & IntelliSense

All `*.angelscript.rules.json` files support full JSON Schema validation and autocompletion in VS Code. Add the `$schema` header at the top of your file:

```json
{
  "$schema": "https://raw.githubusercontent.com/Gaftherman/angelscript-lsp/main/schemas/angelscript.rules.schema.json"
}
```
Or use the relative path if working within the repository:
```json
{
  "$schema": "../schemas/angelscript.rules.schema.json"
}
```

With the schema attached, VS Code provides instant hover descriptions and autocompletion for every rule property!
