# Change Log

All notable changes to the "angelscript-lsp" extension will be documented in this file.

Check [Keep a Changelog](http://keepachangelog.com/) for recommendations on how to structure this file.

## [0.7.7-exp.4] - 2026-09-10

### Added

- Client-side configuration support for `angelscript.enableVirtualMixinDocuments` in `package.json` under resource scope.
- Client launch argument forwarding: passing `--enable-virtual-mixin-documents=true` to the language server process during initialization when enabled.
- Setting `angelscript.inlayHints.suppressWhenArgumentMatchesName` exposed in the client configuration schema and forwarded as `--inlay-hints-suppress-when-argument-matches-name`.
- Full internationalization and localization across English (`package.nls.json`) and Spanish (`package.nls.es.json`) schemas for all new configuration properties.

### Changed

- Updated client setting wiring validation (`check-settings-wired.mjs`) to enforce 100% translation key alignment and argument verification across all 72 configuration options.

## [0.7.7-exp.3] - 2026-09-10

### Added

- Language Server Protocol Call Hierarchy provider (`textDocument/prepareCallHierarchy`, `callHierarchy/incomingCalls`, `callHierarchy/outgoingCalls`):
  - Resolves target functions and method symbols under the cursor, including synthesized mixin members.
  - Incoming calls query workspace-wide call sites targeting functions or synthesized host overloads.
  - Outgoing calls traverse AST function bodies; synthesized host callers resolve back to originating mixin declarations to locate outbound invocations accurately.
- Virtual Mixin Document infrastructure scaffolding behind `--enable-virtual-mixin-documents=[true|false]`:
  - Added URI helper and schema generator for `angelscript-virtual://<host_class>/<mixin>.as`.
  - Added `virtualFileUri` metadata field on `Symbol` instances to support future AST-level virtual document expansion.

### Fixed

- CodeLens mixin deduplication: grouped symbols sharing exact declaration ranges to emit strictly one CodeLens button per mixin method declaration.
- Aggregated reference counts across the mixin declaration and all synthesized host class overloads, eliminating stacked duplicate CodeLens lines and matching the true deduplicated reference list.
- Cascading peer open-document re-analysis debouncing: introduced a 250ms coalescing window with steady-clock tracking (`m_peerAnalysisTimestamps`) in `ReanalyseOpenDocuments` and `DidSave` cascades, preventing re-analysis storms across peer documents such as `base.as`.

### Changed

- Transitioned default setting for parameter inlay hints: `inlayHintsSuppressWhenArgumentMatchesName` is now `false` by default, displaying parameter hints for identifiers matching argument names unless explicitly suppressed.

## [0.7.7-exp.2] - 2026-09-10

### Added

- Overload-aware Go-to-Definition (`FilterOverloadsForCall`):
  - Callee argument type scoring deduces expression types via `ResolveExpressionType` and ranks candidates with `ResolveBestOverload`.
  - Arity and parameter count fallback heuristic scoring when exact types cannot be resolved.
- Parameter inlay hints for `BaseClass` method invocations with hierarchy traversal that bypasses mixins to target true base classes.
- Parameter inlay hints for global and utility objects (`Math.MakeVectors`).

### Fixed

- Mixin origin navigation: jumping to synthesized mixin methods now navigates directly to the mixin's origin declaration file URI and precise line range rather than host class locations.
- Document symbol erasure and invalidation: host class synthesized member cleanup now erases synthesized symbols unconditionally during document updates, preventing stale symbol accumulation.

## [0.7.7-exp.1] - 2026-09-10

### Performance

- Diagnostic checker bypass for `.predefined` engine stubs, reducing workspace stub indexing overhead by over 95%.
- Save Storm elimination: implemented an interface hash guard utilizing an FNV-1a 64-bit ABI fingerprint to prevent cascading re-analysis when public signatures remain unmodified.
- Eradicated linear `symbolTable.ForEachSymbol` iterations in hot performance paths (hover evaluation, document highlights, and enum member resolution) in favor of keyed bucket lookups.
- Added high-resolution microsecond telemetry instrumentation across workspace indexing and document analysis stages.

### Fixed

- Native compiler parity verified via `asharness.exe` for isolated expression statements (`null;`, integer literals, floating-point literals, and boolean literals).
- Predefined stub parsing robustness and handle-to-reference binding resolution.

## [0.2.0]

### Fixed

- Diagnostics refresh while you type again. A pull request that found no answer for the text in
  hand queued the document and told the editor to ask again, and queueing restarted the analysis
  debounce - so an editor polling faster than that held the analysis off indefinitely and the file
  only refreshed when it was saved.
- Toggling a setting no longer reports "Sending notification workspace/didChangeConfiguration
  failed". Two listeners were watching the same event and one restarted the server while the other
  was still pushing configuration into it. Restarts also queue instead of racing.
- `as-err-null-non-handle` is reported as an error, which is what the compiler calls `int x = null`.
  `as-err-undeclared-identifier` is renamed `as-warn-undeclared-identifier`: the hedge behind it is
  real, and the name was the wrong half to keep.
- A property backed by `get_X`/`set_X` accessors is offered by completion and described by hover
  under the name that compiles, honouring `asEP_PROPERTY_ACCESSOR_MODE`.
- The notification about several predefined stubs now carries a button that opens the picker,
  instead of naming a command to go and find.

### Added

- A workspace with several `as.predefined` stubs loads one - the first in path order - and says
  which, rather than merging them all and warning about the duplicate declarations that follow.
  `angelscript.predefined.active` set to `all` restores the merge.
- The settings UI and every message the extension shows are localised; Spanish ships with it.
- Clicking the status bar item offers the server log, a restart, and the stub picker.
- Accessor portability, accessor disabled and bool conversion hints are on by default.

### Changed

- The extension is bundled into a single file and no longer waits for the language server handshake
  before finishing activation: 126 module loads became 1.

## [0.1.0]

- Initial release
