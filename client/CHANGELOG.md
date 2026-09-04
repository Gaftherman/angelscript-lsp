# Change Log

All notable changes to the "angelscript-lsp" extension will be documented in this file.

Check [Keep a Changelog](http://keepachangelog.com/) for recommendations on how to structure this file.

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
