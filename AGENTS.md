# AngelLSP Engineering Manual

AngelLSP is a C++20 language server for AngelScript (`.as`). It uses Tree-Sitter to parse source and index syntax for semantic tokens and symbol analysis.

## Contents

- [Architecture and Code Contracts](#1-architecture-and-includes) (§§1–5)
- [Commits, Testing, and Prohibitions](#6-commits-and-git-hygiene) (§§6–8)
- [Quality Gates](#9-code-quality-and-gates) (§9)
- [Localization, Releases, and Layout](#10-internationalization) (§§10–11)
- [Planner and Executor](#12-planner-and-executor) (§12)

## How to Read This Document

- §§1–2 define dependencies and concurrency. §§3–5 govern signatures, Tree-Sitter, and Doxygen.
- §§6–7 cover commits and testing. §8 lists the 16 prohibitions. §9 maps gates to rules. Rules without gates still require review.
- §§10–11 cover localization, releases, and layout. §12 defines the Planner and executor agent roles and performance acceptance.
- Keep rule identifiers stable, including `Rule 1.2`, `4.7`–`4.11`, and `PROHIBITION n`. Scripts, the ledger, and changelogs cite them. Append new rules at the end of their section. Deprecate by editing the text and retaining the number.

## 1. Architecture and Includes

Paths in this section are relative to `server/src/`. The include gate enforces the layer matrix (§9.7).

| Layer | Source | May Include | Must Never Include |
| --- | --- | --- | --- |
| 1. Core and Config | `config/`, `document/`, `i18n/`, `parser/`, `utils/` | Own layer and standard libraries | Layers 2–4 |
| 2. Analysis | `analysis/` | Layer 1 and standard libraries | Layers 3–4 |
| 3. Features | `features/<feature_name>/` | Layers 1–2 | Sibling features and Layer 4 |
| 4. Server and LSP | `lsp/`, `main.cpp` | Layers 1–3 | — |

Place semantic target, type, scope, and overload resolution services in Layer 2. See PROHIBITIONS 3 and 4 for feature isolation and shared data models. Keep `analysis/` and `parser/` independent of the LSP protocol library (`<lsp/...>`). Shared utilities such as `utils/LspLogger.h` and `i18n/i18n.h` may be included directly.

- **Rule 1.2 — Include Order.** Put every include at the top of the C++ file, before declarations and code. Put quoted project headers before angle-bracket system and library headers.
- **Rule 1.3 — Semantic Mediator.** Layer 2 checkers and resolution code must access `SymbolTable`, `ScopeTree`, and `NodeIndex` through `analysis/mediator/SemanticMediator.h`.

### 1.1. Analysis Sub-Layers

See the directory listings for the files in each sub-layer.

| Sub-Layer | Directory | Responsibility |
| --- | --- | --- |
| 2.1 Index | `analysis/index/` | First-pass AST indexing, lexical structure, immutable facts, and their extractors |
| 2.2 Types | `analysis/types/` | Type representation, sanitization, conversion values, and caches |
| 2.3 Diagnostics | `analysis/diagnostics/` | Diagnostic codes, context, and suppression |
| 2.4 Documentation | `analysis/documentation/` | Doc comments, Markdown, and signature formatting |
| 2.5 Resolution | `analysis/resolution/` | Semantic targets, types, overloads, and related lookups |
| 2.6a Rules | `analysis/rules/` | Semantic language rules |
| 2.6b Checkers | `analysis/checkers/` | Semantic verification, including `null_safety/` |
| 2.7 Mediator and Coordinator | `analysis/mediator/` and `analysis/` root | Semantic mediator and analysis coordinator |
| 2.8 Formatting | `analysis/formatting/` | Protocol-free lexer-based source and predefined stub formatting. Pass `.clang-format` layout options as `FormatLayoutOptions`. |
| 2.9 Completion | `analysis/completion/` | Protocol-free context, candidates, and ranking. Use `analysis/index/CompletionFacts.h` and `analysis/resolution/CompletionReceiverResolution.h`. `features/completion/` maps `CompletionEntry` to LSP items. |
| 2.10 Code Action | `analysis/code_action/` | Protocol-free code actions and refactorings. Use `analysis/index/*Facts.h` for syntax. `features/code_action/` maps `CodeActionEntry` to LSP actions. |

Use `*Facts` for first-pass AST-free values and extractors, `*Internal.h` for headers private to their directory, `*Common.h` for helpers shared by one checker family, and `<Name><Aspect>.cpp` for a split under Rule 4.5.

### 1.2. Server Sub-Layers

| Sub-Layer | Directory | Responsibility |
| --- | --- | --- |
| 4.1 State | `lsp/state/` | In-memory document and workspace state |
| 4.2 Pipeline | `lsp/pipeline/` | Scheduling and diagnostics pipeline |
| 4.3 Services | `lsp/services/` | Internal server domain services |
| 4.4 Handlers | `lsp/handlers/` | Protocol requests and notifications |
| 4.5 Server Core | `lsp/` root | Server coordination |

## 2. Concurrency and AST Lifetime

1. **Tree Ownership.** `DocumentStore` must never return a raw `TSTree*` that outlives its reader lock. When accessing ASTs, retain a `std::shared_ptr<const document::Document>` or use a thread-local `ts_tree_copy()`. See PROHIBITION 1.
2. **Thread Exceptions.** Wrap every secondary thread in a top-level `try/catch`. Log fatal errors to disk with `MultiFileLogger` and shut down gracefully. See PROHIBITION 2.
3. **Node Safety.** Check `!ts_node_is_null(node)` before calling `ts_node_type()`, `ts_node_start_byte()`, or accessing children.
4. **Thread Isolation.** Never share `TSTree*` across threads without `ts_tree_copy()`. Test concurrent mutation and reads with randomized workloads.

## 3. Function Signatures

1. **Parameter Limit.** Use at most four parameters. Bundle additional inputs in an immutable domain struct, for example `struct <Feature>Request`. See PROHIBITION 8.
2. **Owned Signatures.** Under `/we4100`, remove unused parameters from declarations, definitions, and all call sites immediately.
3. **External Signatures.** Mark unused parameters required by overrides, Tree-Sitter callbacks, and generic STL visitors as `[[maybe_unused]] Type name`. Keep names for Doxygen and IDE hovers. No boilerplate `@note` is required.
4. **Parameter Names.** Never leave a parameter unnamed or comment out its name. See PROHIBITION 9.

## 4. Tree-Sitter and Source Structure

The first pass extracts immutable facts with spans and IDs into `NodeIndex`. The second pass reads those facts. A hot path runs per node or identifier across a document or per request item, such as a code lens target or reference.

1. **Structure Extraction.** Reuse the first-pass cursor or index for document-wide work. Keep precompiled S-expression queries for targeted extraction and collectors awaiting migration. See PROHIBITION 6.
2. **Flat Traversal.** For a full subtree, use a flat `TSTreeCursor` loop with `ts_tree_cursor_goto_first_child` and `ts_tree_cursor_goto_next_sibling`. See PROHIBITION 7.
3. **Feature Queries.** Get syntax through Layer 2 facts and analysis APIs. See Rule 4.10 and PROHIBITION 5.
4. **Cohesive C++.** Use domain structs, overloads, and interfaces with Doxygen (§5). The only size limits are §3.1 and Rule 4.5. Lizard, cyclomatic limits, and per-function line limits are retired. See PROHIBITION 8.
5. **Production Source Size.** Target at most 300 net lines per production `.cpp` file. Split larger monolithic files into cohesive units with one responsibility each.
6. **Root-Cause Fixes.** Fix edge cases in grammar queries, symbol resolution, or type rules. Never hardcode name comparisons as a workaround. See PROHIBITION 16.
7. **Parent Navigation.**
   - Use `parser::GetRootNode(node)` for $O(1)$ root access. Never use an unbounded parent climb.
   - For allowed upward traversal, use the null-safe `parser::GetParent(node)`. Raw `ts_node_parent` is confined to `server/src/parser/ASTUtils.h`.
   - Never climb parents in hot-path collectors such as `LocalScopeCollector` and `SymbolCollector`. Never use iterative parent climbs or `ts_node_type` string comparisons in hot-path collectors, query passes, or token classification. Use downward query matching, container stacks, byte-range containment, or cached `TSSymbol` IDs. Where a bounded parent lookup is permitted, use fewer than 16 hops through shared parser helpers. The AST gate rejects hardcoded depth comparisons in callers.
   - Parent lookup costs $O(\text{depth} \times \text{children per level})$. Per-node climbs across a long sibling list are expensive. Carry ancestor context on a stack in one flat cursor pass, as in `IsolationIdentifierFacts`. Keep speed rewrites only under §12.4.
8. **Shared Architecture APIs.**
   - Use `ScopeTree::FindInnermostScope(byte)` for local definitions and `NodeIndex` lexical records for containers and namespace imports. Positional lookup uses binary search followed by enclosing links. Its cost includes scopes visited and names returned. An import applies within its owning lexical scope.
   - Follow Rule 4.7 for parent and root access. Read node text with `parser::NodeText(node, source)` as a `std::string_view`. Copy it with `std::string(...)` only to outlive the source. Read operators with `parser::OperatorSpelling(node)`.
   - Use `utils::ScopedNameUtils` for allocation-free scoped names: `StripGlobalPrefix`, `HasLeadingGlobalPrefix`, `ParentScopeView`, `SplitScopedName`, `ForEachEnclosingScope`, and `AreEquivalentGlobalOrScopedNames`.
   - Use `SymbolTable::FindMemberSymbolPtr(container, name)` or `SymbolTable::FindMemberSymbols(container, name)` for members. Use `SymbolTable::HasScopedSymbol(scope, name)` or `SymbolTable::IsReachableInContainerScope(name, containerScope)` for reachability. Never concatenate scoped lookup keys with `+ "::" +`.
   - Extract structure with cached `GrammarSymbols` and `NodeIndex::ForEachNodeOrdered(std::span<const TSSymbol>)` without string conversions or runtime map lookups.
   - Reuse `features/common/FeatureSharedUtils.h` for protocol mapping and geometry: `ToLspRange`, `SameRange`, `FindIdentifierAtPosition`, and `ToLspSymbolKind`.
   - Use `analysis::TypeQualifiers` and `server/src/analysis/types/TypeSanitization.h` for zero-allocation type modifier operations: `StripLeadingConst`, `StripTrailingDecorationsView`, and `TrimTypeWhitespace`.
9. **First-Pass Context (Rule 4.9).** Extract facts in one $O(N)$ document-wide pass per syntax revision. A captured node ID can support $O(1)$ access. State the actual cost of positional and multi-result queries. Migrated second-pass consumers must use facts and worklists for flow and expression analysis. The AST gate checks direct calls in migrated checkers. Measure runtime calls through their dependencies too. Obtain the LSP syntax revision through `ResolveSyntaxRevision` and reuse exact parser input on the owning thread. Stay within the retained syntax cache budget and give open revisions priority over closed entries.
10. **Pass Boundary (Rule 4.10).**
    - Only `server/src/parser/` and `server/src/analysis/index/` may use Tree-Sitter types, `ts_*`, or `parser/ASTUtils.h`. `NodeIndex` must expose immutable, AST-free facts, symbols, spans, and tokens to the second pass. Never expose raw access such as `->Root()`, callbacks taking `TSNode`, or traversals.
    - Second-pass features, analysis consumers, LSP, and utilities must use $O(1)$ or $O(\log N)$ indexed lookups or flat passes over indexed facts. Unbounded recursion is prohibited.
    - Use `std::string_view` for inspection, parameters, and paths. Never use read-only `const std::string&` parameters, lookup through temporary `std::string(...)`, or copies made only to inspect. Use transparent hashing from `utils/TransparentHash.h`. Owning members, containers, return values, and sinks that retain values are allowed. See §9.7 for the verifier.
11. **Utility Migration (Rule 4.11).** Never copy a shared utility body into its callers. For utilities using `ts_*` or `std::string` in `utils/`, `features/common/FeatureSharedUtils.h`, `analysis/resolution/SemanticHelpers.h`, or `analysis/index/NodeIndex.h`:
    1. Trace every caller with clangd `find_references`.
    2. Capture needed facts in the first pass through `NodeIndex`, `SymbolCollector`, or `LocalScopeCollector`.
    3. Add an overload accepting first-pass data and `std::string_view`.
    4. Migrate callers one at a time.
    5. Delete the legacy overload only when it has zero references.

## 5. Doxygen

Document public classes, structs, member variables, functions, and feature contracts in English with `/** ... */` blocks. Give substantive descriptions. Never use templates that simply repeat types or parameter names.

- Write a single-line `@brief` ending with a period.
- Use `@param[in/out]` with an explicit direction, name, and purpose.
- Use `@return` to describe results, including `std::nullopt` and empty states.
- Use `@note` or `@warning` for concurrency guarantees and AST lifetimes.

## 6. Commits and Git Hygiene

Use `<type>(<scope>): <short imperative description>` without a trailing period. Group one coherent change and one orchestration job per commit (§12). Add host-requested attribution trailers at the end of the body. Never commit debug output such as `std::cout`, `printf`, or temporary traces.

- Types: `feat`, `fix`, `test`, `perf`, `refactor`, `style`, `docs`, `chore`.
- Scopes: `core`, `parser`, `analysis`, `index`, `features`, `server`, `client`, `harness`, `scripts`, `tests`, `docs`.

## 7. Testing Invariants

1. **Fixtures.** Generate identifiers and paths to test general behavior. Use fixed fixtures for precise regressions or external corpus parity. Record seeds when generated structure matters for reproduction.
2. **Path Containment.** Test resolution in temporary sandboxes with randomized traversal at multiple depths. Fixed traversal strings alone are insufficient.
3. **Transport.** Enforce `MAX_LSP_PAYLOAD_SIZE = 16 MB` in JSON-RPC. Reject oversized payloads and malformed headers without allocations or crashes.
4. **Graph Invalidation.** Test `WorkspaceIncludeGraph` invalidation on randomized DAGs. Assert deduplicated visits in $O(V + E)$.
5. **Refactor Equivalence.** Run the full suite and the differential corpus against the AngelScript oracle with `server/scripts/differential-validate.py`. Expect zero discrepancies and zero oracle contradictions. When replacing an algorithm, compare against a copy of the old one on generated inputs and record the seed.
6. **Struct Layout.** Put fact and index size checks in `FactLayoutSizeTest.cpp`. Avoid scattered `static_assert` checks.
7. **Benchmarks.** Mark benchmark cases `doctest::skip` and run with `-ns`. Report timings without assertions on elapsed time. Base memory conclusions on process max RSS.

### 7.1. Deterministic Concurrency Tests

1. **No Sleep or Polling.** Never use `std::this_thread::sleep_for()`, loops around `std::this_thread::yield()`, or counter-based polling in tests. Never assert that an async task started, finished, or failed based on elapsed time.
2. **Event Synchronization.** Use `std::promise<T>` and `std::future<T>::get()` for one-shot completions and exceptions, `std::binary_semaphore` or `std::counting_semaphore` for handoffs, and `std::latch` or `std::barrier` for rendezvous. Signals must unblock waiting threads without artificial latency.
3. **External Timeouts.** Use CMake test properties such as `set_tests_properties(<target> PROPERTIES TIMEOUT 10)` for hang and deadlock protection. Never write in-test watchdogs, sleep loops, or timeout retries.

## 8. Prohibitions

1. **PROHIBITION 1 — Raw AST Pointers.** Never return or store a raw `TSTree*` without an enclosing reader lock or owning `std::shared_ptr<const document::Document>`. See §2.
2. **PROHIBITION 2 — Uncaught Thread Exceptions.** Never let an exception escape a `std::thread` boundary. A resulting `std::terminate()` is a critical bug. See §2.
3. **PROHIBITION 3 — Cross-Feature Includes.** Never include sibling feature headers from `server/src/features/`. Put cross-cutting capabilities in Layer 2.
4. **PROHIBITION 4 — Duplicate Analysis Models.** Never copy core models such as `TargetDescriptor` or `TargetKind` to bypass layer rules.
5. **PROHIBITION 5 — Ad Hoc Feature Lexers.** Never slice text, count parentheses, or write ad hoc lexers to inspect expressions in Layer 3. Use indexed syntax facts or Layer 2 type resolution (§4.3, Rule 4.10).
6. **PROHIBITION 6 — Indexed Child Loops.** Never loop over children with `ts_node_child`. Use precompiled S-expressions or a flat cursor.
7. **PROHIBITION 7 — Recursive AST Traversal.** Use iterative cursors or explicit worklists. Surface a partial result if a budget prevents completion. Never discard valid input silently through arbitrary depth limits.
8. **PROHIBITION 8 — Artificial Fragmentation.** Never split cohesive functions into micro-routines or create synthetic parameter bags to evade limits. Use domain structs, overloads, and interfaces or inheritance where appropriate. Follow §5.
9. **PROHIBITION 9 — Dead or Unnamed Parameters.** Follow §3 for owned and external signatures under `/we4100`.
10. **PROHIBITION 10 — Fixture-Specific Production Behavior.** Verify general behavior with generated inputs. Keep fixed regressions where source spelling or layout is contractual.
11. **PROHIBITION 11 — Hardcoded Text and Locale Branching.** Follow §10 for user-facing text and locale selection.
12. **PROHIBITION 12 — Unbounded and Hot-Path Parent Climbs.** Follow Rule 4.7.
13. **PROHIBITION 13 — Hardcoded Types and Blind Namespace Matching.** Never hardcode predefined or add-on type names such as `"weakref"`, `"const_weakref"`, `"string"`, or `"dictionary"` in Layer 2 conversion rules, overload rankings, or type checkers. Resolve capabilities through `SymbolTable` and template substitution with `BindTemplateArguments` and `SubstituteTemplateParameters`. Never compare only `LastScopeSegment` across different explicitly qualified scopes.
14. **PROHIBITION 14 — Anonymous Namespaces, Using Declarations, ODR Safety, and Doxygen.** Never introduce `namespace {}`, `using namespace`, or `using <ns>::<symbol>` in implementation files. Keep shared structs in canonical headers and local helper structs in private class scope. Follow §5 for substantive Doxygen.
15. **PROHIBITION 15 — Forward Declarations and Redundant Aliases.** Include canonical headers in `server/src/`. Never write standalone class or struct forward declarations or alias a canonical struct redundantly in the same namespace.
16. **PROHIBITION 16 — Raw Literal Comparisons.** In Layers 2–4, use canonical classifications for types, keywords, and literals. Use `TypeKind` from `ParameterInformation`, `Symbol`, or expression facts, `parser::keywords::*`, `GrammarSymbols`, `TypeSanitization` predicates such as `HasLeadingConst` and `IsHandleTypeView`, and `ParseTemplateType`. If no canonical form exists, add a predicate and migrate every caller. External text parsing, including `.clang-format`, DAP, and JSON, is exempt. See §9.8 for the ratchet.

## 9. Code Quality and Gates

Apply these rules to C++ changes in `server/src/`. The Planner runs the verification in §12.

1. **Duplication.** Keep jscpd at or below 3%. Share logic across checkers, rules, and features through Layer 1 utilities or Layer 2 analysis helpers.
2. **Readability.** Prefer early returns and structured switch dispatch. Avoid nesting deeper than three levels. Decompose by responsibility and follow Rule 4.4.
3. **Cppcheck.** Resolve uninitialized variables, null dereferences, resource leaks, and scope-escape defects.
4. **Quality Gate.** The Planner runs `python server/scripts/check-quality.py` before the user commits. Inspect failures, fix their causes, and re-verify.
5. **Release Build.** After running `format-code.py` or Python refactoring scripts, the Planner must run `cmake --build build --config Release` and verify zero errors and warnings under `/W4 /WX /we4100` before any commit or push.
6. **Refactor Ledger.** Record every struct unification, function consolidation, flag reparsing elimination, and layout compaction in `docs/REFACTOR_LEDGER.json`. The Planner validates with `python server/scripts/check-refactor-ledger.py --check-diff develop` and `python server/scripts/audit-codebase-redundancy.py`.

### 9.7. Gate Map

`server/scripts/check-quality.py` orchestrates the checks below. It runs the external tools and format check when available.

| Gate | Coverage |
| --- | --- |
| `check-layer-includes.py` | §1 layer matrix and protocol isolation |
| `check-clean-signatures.py` | §3 and PROHIBITION 9 |
| `verify_pass2_invariants.py --strict-string-view` | Rule 4.10. The script holds its folder list in `SECOND_PASS_FOLDERS`. |
| `check-diagnostic-codes.py` | Diagnostic code registry |
| `check-client-l10n.py`, `sync-readme-settings.py --check` | §10 and settings catalog |
| `check-grammar-names.py`, `check-grammar-pin.py` | Rule 4.8 and the grammar pin |
| `check-ast-traversal.py` | Rules 1.2 and 4.7–4.10, AST traversal checks for Rules 4.1–4.2, and PROHIBITIONS 6, 7, 12–15 |
| `check-refactor-ledger.py` | §9.6 |
| `check-raw-literal-compare.py` | PROHIBITION 16 |
| jscpd, cppcheck | §§9.1 and 9.3 |
| ast-grep with `server/rules/sgconfig.yml` | AST and allocation patterns |
| `format-code.py --check` | Clang-Format style |

The `angel_lsp_anti_slop_gate` build target runs the AST traversal gate before compilation. Add new gates to this table in the same change.

### 9.8. Ratchet Policy

Compare per-file counts against `server/rules/raw-literal-baseline.json`. Counts may only decrease. Run the gate with `--update` in the same change when lowering a count. Promote zero baselines to hard gates.

### 9.9. Ledger IDs

Use one ledger entry per orchestration job with an ID like `REF-P20-ISOLATION-IDENT-PASS` (`REF-<phase>-<topic>`) and a valid action from existing entries. Remove the entry if the job is reverted.

## 10. Internationalization

1. **User-Facing Text.** Externalize all user-facing prose, including hover Markdown, diagnostics, code actions, document links, and notifications. Put server strings in `server/src/i18n/locales/en.json` and `server/src/i18n/locales/es.json`. Use `angel_lsp::i18n::FormatMessage(i18n, key, fallbackPattern, ...)` for server messages and all dynamic text formatting on the server. Use `l10n.t()` with the client bundles. Never hardcode English or Spanish prose in Layer 2 or 3.
2. **Catalog Parity.** Add matching server keys and format fields to both catalogs. CMake validates them and generates static message tables in the build directory. Keep `client/l10n/bundle.l10n.json` and `client/l10n/bundle.l10n.es.json` in one-to-one sync.
3. **Locale Resolution.** Resolve server locales centrally through `angel_lsp::i18n::I18n`. Never branch on language directly in feature handlers, including with ternary expressions.

## 11. Releases and Repository Hygiene

1. **Version Sections.** Every released tag must have an exact version section in `CHANGELOG.md`. Mirror it to `client/CHANGELOG.md` with `server/scripts/sync-changelogs.py`.
2. **Release Bodies.** Use `server/scripts/extract-release-notes.py` to publish the exact detailed Markdown entry, preserving the git comparison link. Never use a blank release body or a generic commit summary. Keep `--generate-notes` as a fallback only. Sync historical releases with `server/scripts/sync-github-releases.py`.
3. **Privacy.** Never commit or publish personal email addresses, local usernames, or machine-specific paths in changelogs, fixtures, or code. Use generic paths or randomized fixtures such as `angel_lsp::test::GenerateRandomSymbolName()`. See §12.6.
4. **Canonical Docs.** Use `AGENTS.md`, `docs/REFACTOR_LEDGER.json`, `docs/GUIDE*.md`, `docs/CONFIGURATION*.md`, `docs/RULES*.md`, and `CHANGELOG.md`. Keep structural refactoring records in the ledger (§9.6). Keep temporary logs, scratch scripts, and ad hoc configuration out of the root.

### 11.5. Repository Layout

| Location | Contents |
| --- | --- |
| Root | Engineering manual, README, contributing guide, changelog, license, shared Git and Clang configuration, and SonarCloud settings (`.sonarcloud.properties` for Automatic Analysis, `sonar-project.properties` for a CI scanner; keep them aligned; tests, fixtures and `server/scripts/` are excluded from analysis) |
| `client/` | VS Code extension, tests, and packaging |
| `server/` | C++ server, tests, and quality tools in `server/scripts/`, including `run-harness.ps1`. AST rules live in `server/rules/ast-grep/` with configuration in `server/rules/sgconfig.yml`. |
| `docs/` | Guides, rules, Spanish repository README and contributing guide, third-party notices, parity corpus guide, and refactor ledger |
| `predefined/` | Bundled predefined declarations and engine rules |
| `schemas/` | Published JSON schema |
| `.github/` | CI workflows and issue templates |

Keep `schemas/angelscript.rules.schema.json` and `client/schemas/angelscript.rules.schema.json` identical. The first is served at the published `$id` URL. The second ships in the VSIX.

## 12. Planner and Executor

1. **Roles.** The Planner designs, briefs, builds, verifies, measures, and decides. The executor agent researches and edits. The current executor is Codex, launched with `codex exec --dangerously-bypass-approvals-and-sandbox` in an isolated worktree. The executor never builds or runs Python gates, ast-grep, cppcheck, or jscpd. Only the user commits or pushes.
2. **Briefs and Jobs.** Use one topic per job and one write job at a time per worktree. The Planner does not edit during the job. Keep the self-contained brief outside the repository. Include the measurement or motivation, cause, exact edits and files, hard rules (§§3–4 and §8), and expected report. Pass only repo-relative names and the brief pointer in the prompt, with stdin closed. Allow twice the measured duration as timeout, typically 8–20 minutes. Finish the job after the executor process exits and the Planner reads its report.
3. **Closing Gates.** Per job, the Planner runs a target-platform Release build with zero errors and warnings under `/W4 /WX /we4100`. Run the full suite, then `format-code.py`, a rebuild, `check-quality.py`, and `verify_pass2_invariants.py --strict-string-view`. Run the differential corpus (§7.5). Add the ledger entry (§9.9) and read the full `git diff` for misplaced `parser/ASTUtils.h`, `->Root()`, avoidable string allocations, and duplication.
4. **Performance Acceptance.** Keep a speed change only if the target row improves by at least 5% at both 5k and 20k lines and no other row worsens by more than about 3% (noise floor). Otherwise revert every file of the job, including tests and the ledger entry, and report the numbers. Use Linux `perf` in the container and a baseline binary built from `git stash`. Interleave four runs per side, or six for noisy rows such as `codeLens`. Compare p50 and p95 medians within the same session. If profiling shows remaining cost spread across parser and index construction, report that limit.
5. **Edit Contracts.** Start every modification from an exact contract, such as first-pass data structures and `std::string_view` overloads. Delegate AST and symbol lookups to the executor. Ask for about 30 lines covering `ts_*` calls, `std::string` use, recursion, and exact call sites.
6. **Privacy.** Keep personal emails, usernames, and machine paths out of briefs, prompts, ledger entries, fixtures, and changelogs. Keep personal information out of requests to external services.
