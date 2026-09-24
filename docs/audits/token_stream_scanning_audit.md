# Token-Stream Scanning Antipattern Audit & Remediation Map

**Project:** AngelScript Language Server (`angel_lsp`)  
**Audit Scope:** `server/src/`  
**Standard:** Rule 3.1 & Rule 8 (Prohibitions 5 & 6) in `AGENTS.md`  
**Date:** September 2026  

---

## 1. Executive Summary

Tree-Sitter parses source code into a Concrete Syntax Tree (CST) with both **named semantic nodes** (e.g. `call_expression`, `identifier`, `type`, `argument_list`, `parameter`) and **anonymous terminal tokens** (e.g. `(`, `)`, `,`, `:`, `;`).

A severe architectural antipattern occurs when AST traversal code discards the tree structure and treats children as a flat, unparsed token stream. Common symptoms in the codebase include:
1. **Manual comma counting (`cType == ","` or `++commas`)** to infer argument counts or active parameter indices.
2. **Buffer-and-split loops** accumulating raw tokens into temporary vectors until hitting a comma `,` or colon `:`.
3. **Punctuation skipping heuristics** (`act == "(" || act == ")" || act == "," || act == ":"`) inside raw `ts_node_child` loops.
4. **Ad-hoc lexical scanners** in Layer 3 tracking bracket and parenthesis depth to count commas in source strings.

### Failure Modes & Latent Bugs
- **Multi-argument template types:** Types like `dictionary<string, int>` or `pair<float, bool>` contain internal commas. Token stream scanning desynchronizes parameter counts and indices when encountering commas inside template argument lists.
- **Nested calls in default arguments:** Default parameter expressions like `void Log(string msg = Format("x, y", 1, 2), int level = 0)` contain inner commas that break naive comma counting.
- **Named arguments:** Named argument syntax (`foo(arg1: val1, arg2: val2)`) introduces extra identifiers and colons `:` that cause off-by-one and off-by-two index skew in argument counting and document highlight indexing.
- **Malformed code during live typing:** Incomplete lists like `func(a, , b)` cause token accumulation buffers to produce empty or invalid groups, risking indexing desynchronization or crashes.

---

## 2. Categorized Inventory of Occurrences

### Category A: Parameter & Argument Indexing (High Severity)

| File | Lines | Current Antipattern | Failure Mode | Target Remediation |
| :--- | :--- | :--- | :--- | :--- |
| `server/src/analysis/CallChecker.cpp` | 85–107 | `CountArgumentsInList`: Counts commas `++commas` and returns `commas + 1`. | Miscounts empty arguments `(,)`, breaks on syntax errors or unexpected tokens. | Replace with `analysis::CountCallArguments` using named child traversal over expressions. |
| `server/src/analysis/CallChecker.cpp` | 146–180 | `GetArgumentNodes`: Iterates with `ts_node_child`, buffers tokens into `currentGroup`, flushes on `childType == ","`, and scans for `:`. | Inefficient token buffering, fails on complex expression shapes. | Replace with `analysis::ExtractCallArguments`. |
| `server/src/analysis/CallChecker.cpp` | 210–246 | `GetArgumentNames`: Buffers tokens into `currentGroup`, flushes on `,`, calls `ExtractArgumentName`. | Duplicate comma buffering loop. | Replace with `analysis::ExtractCallArguments`. |
| `server/src/analysis/CallChecker.cpp` | 1105–1135 | `PartitionArguments`: Buffers tokens into `currentGroup`, flushes on `,`. | Third duplicate token buffering loop in the same file. | Replace with `analysis::ExtractCallArguments`. |
| `server/src/analysis/SemanticHelpers.cpp` | 4116–4155 | `FindLambdaParamIndex`: Increments `currentIndex` on `cType == ","`. | Desynchronizes on malformed parameter lists or nested commas. | Refactor using `analysis::ExtractLambdaParameters`. |
| `server/src/features/document_highlight/DocumentHighlightHandler.cpp` | 395–414 | `FindArgumentIndex`: Increments `argIndex` on any token not `(`, `)`, `,`. | Named arguments (`name: expr`) increment `argIndex` three times (for name, colon, and expression). | Use `analysis::ExtractCallArguments` and match `leaf` against argument AST bounds. |
| `server/src/features/signature_help/SignatureHelpHandler.cpp` | 200–258 | `CalculateActiveParameter` & `UpdateDepth`: Custom string scanner tracking brackets and incrementing `activeParam` on `,`. | Prohibition 5 violation (ad-hoc lexer in Layer 3). Desynchronizes on nested template brackets, shifts `>>`, or string escape oddities. | Replace with AST-based argument containment / cursor interval resolution via `ExtractCallArguments`. |

---

### Category B: Manual Delimiter & Punctuation Skipping (Medium Severity)

| File | Lines | Current Antipattern | Failure Mode | Target Remediation |
| :--- | :--- | :--- | :--- | :--- |
| `server/src/analysis/LocalScopeCollector.cpp` | 297–320 | `CountArguments`: Cursor loop checking `act == "(" \|\| act == ")" \|\| act == "," \|\| act == ":"`. | Ad-hoc token skipping, increments on any unhandled token. | Delegate to `analysis::CountCallArguments`. |
| `server/src/analysis/LocalScopeCollector.cpp` | 660–700 | `FindLambdaParamTypeNode`: Cursor loop resetting on `cType == ","`, skipping `(`, `)`. | Breaks if lambda parameter contains unexpected punctuation. | Delegate to `analysis::ExtractLambdaParameters`. |
| `server/src/analysis/SemanticHelpers.cpp` | 3711–3750 | `ReadLambdaParameters`: Raw `ts_node_child` loop checking `text == ","`, `text == "("`, `text == ")"`. | Extracts text strings for every punctuation token in loop. | Refactor to `analysis::ExtractLambdaParameters`. |
| `server/src/analysis/TargetResolution.cpp` | 504–524 | `CountCallArguments`: `ts_node_child` loop checking `act == "(" \|\| act == ")" \|\| act == "," \|\| act == ":"`. | Duplicate of `LocalScopeCollector.cpp` loop. | Delegate to `analysis::CountCallArguments`. |
| `server/src/features/definition/DefinitionHandler.cpp` | 180–198 | `ExtractCallArgumentTypes`: `ts_node_child` loop checking `ct == "(" \|\| ct == ")" \|\| ct == "," \|\| ct == ":"`. | Duplicated across 3 feature handlers; manual token checking. | Replace with Layer 2 `analysis::ExtractCallArgumentTypes`. |
| `server/src/features/hover/HoverHandler.cpp` | 683–702 | `ExtractCallArgumentTypes`: Duplicate of `DefinitionHandler` loop. | Sibling feature duplication & manual token checking. | Replace with Layer 2 `analysis::ExtractCallArgumentTypes`. |
| `server/src/features/inlay_hint/InlayHintHandler.cpp` | 338–358 | `ExtractCallArgumentTypes`: Duplicate of `DefinitionHandler` loop. | Sibling feature duplication & manual token checking. | Replace with Layer 2 `analysis::ExtractCallArgumentTypes`. |
| `server/src/features/inlay_hint/InlayHintHandler.cpp` | 495–535 | `CollectCallArgumentHints`: Checks `type == ","`, `type == ":"`, clears name on comma. | Fragile state machine over flat child tokens. | Refactor to use `analysis::ExtractCallArguments`. |

---

### Category C: Raw Index Child Loops (`ts_node_child` over Unnamed Tokens)

Multiple locations in `server/src/` iterate raw `ts_node_child` instead of flat `TSTreeCursor` or `ts_node_named_child`:
- `CallChecker.cpp`: 5 loops over `argumentList`.
- `DefinitionHandler.cpp`, `HoverHandler.cpp`, `InlayHintHandler.cpp`: Call argument iteration loops.
- `DocumentHighlightHandler.cpp`: `FindArgumentIndex` loop.
- `SemanticHelpers.cpp`: `ReadLambdaParameters` loop.

All of these will be replaced with single-pass, flat `TSTreeCursor` named-node traversals or precompiled queries.

---

## 3. Standard Architecture & Replacement Specifications

### Specification 1: Unified Call Argument Extraction (`Layer 2: analysis/SemanticHelpers`)

```cpp
struct CallArgumentInfo
{
    std::string name;       ///< Argument name if named (e.g. "param" in "param: value"), else empty.
    TSNode exprNode{};      ///< AST node of the argument value expression.
    TSNode nameNode{};      ///< AST node of the argument name identifier if named, else null.
    uint32_t index = 0;     ///< Zero-based argument index in the call.
};

/**
 * @brief Structurally extracts call arguments from an argument_list AST node.
 *        Never scans commas or delimiters manually; relies strictly on named child AST structure.
 * @param[in] argumentList AST node representing argument_list.
 * @param[in] sourceCode Document source text for extracting argument names.
 * @return Ordered list of CallArgumentInfo structs.
 */
std::vector<CallArgumentInfo> ExtractCallArguments(TSNode argumentList, std::string_view sourceCode);

/**
 * @brief Returns the number of argument expressions present in an argument_list.
 * @param[in] argumentList AST node representing argument_list.
 * @return Count of actual argument expressions.
 */
size_t CountCallArguments(TSNode argumentList);

/**
 * @brief Deduces expression types for all arguments in a call expression.
 * @param[in] callNode AST node representing the call_expression.
 * @param[in] scope Lexical scope at the call site.
 * @param[in] symbolTable Global symbol table.
 * @param[in] sourceCode Document source text.
 * @param[in] uri Document URI.
 * @return Ordered vector of deduced argument type strings.
 */
std::vector<std::string> ExtractCallArgumentTypes(TSNode callNode, const Scope* scope,
                                                  const SymbolTable& symbolTable,
                                                  std::string_view sourceCode,
                                                  std::string_view uri);
```

### Specification 2: Unified Lambda Parameter Extraction (`Layer 2: analysis/SemanticHelpers`)

```cpp
struct LambdaParamASTInfo
{
    TSNode typeNode{};      ///< AST node for declared type (if typed), else null.
    TSNode nameNode{};      ///< AST node for parameter identifier (if named), else null.
    TSNode startNode{};     ///< Leading node for the parameter (type or identifier).
    std::string name;       ///< Parameter identifier name.
    std::string typeName;   ///< Declared type name string if present.
    uint32_t index = 0;     ///< Zero-based parameter position in lambda header.
};

/**
 * @brief Structurally extracts parameters from a lambda_parameter_list AST node.
 * @param[in] lambdaParamList AST node representing lambda_parameter_list.
 * @param[in] sourceCode Document source text.
 * @return Ordered vector of LambdaParamASTInfo structs.
 */
std::vector<LambdaParamASTInfo> ExtractLambdaParameters(TSNode lambdaParamList, std::string_view sourceCode);
```

---

## 4. Verification & Invariant Enforcement Plan

1. **New Invariant Suite:** `server/tests/AstStructuralTraversalParityTest.cpp`:
   - `Multi-Argument Template Types`: `function( dictionary<string, int> dict, int flag, pair<float, bool> p )`
   - `Default Arguments Containing Commas`: `void Process( int a, string b = Format("x, y", 1, 2), int c = 0 )`
   - `Named Arguments in Call Expressions`: `Draw( color: Color(255, 0, 0), thickness: 2 )`
   - `Malformed Parameter Lists`: `function( int a, , float b )` (no hang, no crash, proper index attribution)
   - `Signature Help Active Parameter Resolution`: Cursor positioned across comma boundaries with nested function calls.
2. **Harness Verification:**
   - 0 token equality inspections on commas/parentheses for indexing.
   - 100% CTest pass rate across all 1,890+ test cases.
   - Zero Layer Matrix or signature violations.
