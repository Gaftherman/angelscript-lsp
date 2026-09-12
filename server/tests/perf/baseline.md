# Performance Baseline Measurements

Recorded before hot-path optimization and compiler modernization work.
Date: 2026-09-12
Branch: test/compiler-modernization
Configuration: Debug (MSVC v143, x64)

## 1. End-to-End & Feature Timings (PerfBaselineTest)
Tested on fixture `server/tests/perf/large_script_3000.as` (3,000 lines) with `predefned/sven.as.predefined` loaded.

| Metric | Baseline Value | Description |
| :--- | :--- | :--- |
| Sven stub load time (`ParserPredefined`) | 1310.82 ms | Cold parse and symbol extraction for `sven.as.predefined` (361 KB) |
| `didOpen` to first `publishDiagnostics` | 3549.14 ms | Time from `didOpen` frame through parser, collectors, and semantic validation |
| Debounced re-analysis after `didChange` | 477.03 ms | Single character change followed by debounced analysis loop stabilization |
| `GetSemanticTokens` full | 314.92 ms | Tokenization of 3,000-line script (6,725 tokens generated) |
| `GetSemanticTokens` delta | 0.29 ms | Delta calculation between previous token stream and edited script (1 edit) |
| Peak resident memory (Working Set) | 80.04 MB | Process memory following stub ingestion and full script analysis |

---

## 2. Analysis Rule Costs (RuleCostTest - 300 Corpus Files)
Measured over 300 files from the AngelScript corpus.

| Analysis Component | Total Time (ms) | Per-File Average (ms) |
| :--- | :--- | :--- |
| Front-End (`parse + collect + scopes`) | 67696.90 ms | 225.66 ms |
| Declaration Rules (`duplicates, class, var, fn, op`) | 904.07 ms | 3.01 ms |
| Control Flow (`CheckControlFlow`) | 1937.96 ms | 6.46 ms |
| Type Conversions (`CheckTypeConversions`) | 27401.00 ms | 91.34 ms |
| Member Access (`CheckMemberAccess`) | 33869.30 ms | 112.90 ms |
| Const Correctness (`CheckConstCorrectness`) | 965.08 ms | 3.22 ms |
| Call Arguments (`CheckCallArguments`) | 19393.80 ms | 64.65 ms |
| Call Graph (`CollectCalls`, not in Analyze) | 1230.53 ms | 4.10 ms |
| **`SemanticAnalyzer::Analyze` (Whole)** | **190801.00 ms** | **636.01 ms** |

### Breakdown by Declaration Module
| Sub-Module | Total Time (ms) |
| :--- | :--- |
| Duplicates | 506.41 ms |
| Class Rules | 581.63 ms |
| Variable Rules | 173.50 ms |
| Function Rules | 164.79 ms |
| Operator Rules | 109.30 ms |

---

## 3. Key Observations & Inefficiencies
1. `SemanticAnalyzer::Analyze` takes 636 ms per file on average, dominated by `CheckMemberAccess` (112.9 ms), `CheckTypeConversions` (91.3 ms), and `CheckCallArguments` (64.6 ms), due to 13 separate full-AST traversals from `ts_tree_root_node`.
2. `RuleIndex::Build` walks the entire workspace symbol table on each analysis, resulting in quadratic overhead as document counts grow.
3. `FindSymbols` copies symbol vectors and strings on every identifier lookup during semantic tokens and analysis.
4. `GetSemanticTokens` spends 315 ms per 3,000-line document evaluating an if-else string comparison chain across captures and performing repeated symbol lookups.
