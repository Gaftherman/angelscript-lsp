# tree-sitter-angelscript

A [tree-sitter](https://tree-sitter.github.io/) grammar for the [AngelScript](https://www.angelcode.com/angelscript/) scripting language.

Provides parsing, syntax highlighting, and code navigation for AngelScript `.as` files in any editor or tool that supports tree-sitter.

## Features

### Language coverage

The grammar covers the full AngelScript syntax:

- **Declarations** - classes (with inheritance, abstract/final/shared modifiers), interfaces, enums, namespaces, mixins, typedefs, funcdefs, imports, using directives
- **Functions** - declarations, constructors, destructors, forward declarations, parameter modifiers (`in`, `out`, `inout`), default values, variadic parameters, function attributes (`override`, `final`, `explicit`, `property`, `delete`)
- **Types** - primitives (`int`, `float`, `bool`, `string`, etc.), templates (`Array<T>`), handles (`@`), const qualifiers, auto type, scoped types (`Mod::Type`), arrays (`int[]`)
- **Statements** - if/else, for, foreach, while, do-while, switch/case, try/catch, return, break, continue
- **Expressions** - full operator precedence (15 levels), assignment operators, ternary, logical (`&&`, `||`, `^^`, `and`, `or`, `xor`), bitwise, identity (`is`, `!is`), exponentiation (`**`), unsigned right shift (`>>>`), handle-of (`@`), prefix/postfix increment/decrement, member access, indexing (with named indices), function calls (with named arguments), cast expressions, lambda expressions
- **Literals** - integers (decimal, hex `0xFF`, octal `0o77`, binary `0b1010`), floats (with exponent and suffix), single/double quoted strings, triple-quoted heredoc strings, booleans, null
- **Virtual properties** - get/set accessors with const and attribute support
- **Handle assignment** - `@handle = obj` (handle-of on the left-hand side) and the `@=` operator
- **Preprocessor directives** - line directives (`#include`, `#if`, `#ifdef`, `#else`, `#endif`, `#pragma`, ...) are parsed as extras, so `#if`/`#endif` pairs can wrap any region of code without producing errors
- **Comments** - single-line (`//`) and block (`/* */`)

### External scanner

An external scanner (`src/scanner.c`) handles template `<>` disambiguation, distinguishing generic types like `Array<int>` from comparison operators. It tracks template nesting depth and uses heuristics to determine whether `<` opens a template argument list or is a less-than operator.

The scanner is tuned for error recovery on incomplete code (e.g. while typing in an editor): an unclosed template argument list like `array<int` is implicitly closed at the end of the line instead of breaking the syntax tree for the rest of the block, and an error sentinel keeps stray `<`/`>` inside error regions from corrupting the template-depth state.

### Query files

| File | Purpose |
|---|---|
| `queries/highlights.scm` | Syntax highlighting -- keywords, types, functions, literals, operators, punctuation |
| `queries/locals.scm` | Scope tracking -- scopes, definitions (variables, parameters, fields, functions, types), and references for local-variable highlighting and smart rename |
| `queries/tags.scm` | Code navigation -- symbol definitions (functions, classes, interfaces, enums, namespaces, properties) and references (function/method calls) |

### Test corpus

159 tests across 8 corpus files covering basics, declarations, expressions, functions, statements, preprocessor directives, error recovery, and type system (including template disambiguation).

## Usage

### Rust

Add to `Cargo.toml`:

```toml
[dependencies]
tree-sitter-language = "0.1.7"
tree-sitter = "0.26.7"
```

```rust
use tree_sitter_angelscript::LANGUAGE;

let mut parser = tree_sitter::Parser::new();
parser.set_language(&LANGUAGE.into()).unwrap();

let source = "void main() { int x = 42; }";
let tree = parser.parse(source, None).unwrap();
println!("{}", tree.root_node().to_sexp());
```

The crate also exports `HIGHLIGHTS_QUERY` (the contents of `queries/highlights.scm`) and `NODE_TYPES` (the contents of `src/node-types.json`).

### Node.js / CLI

```bash
npm install
npx tree-sitter generate
npx tree-sitter parse examples/sample.as
npx tree-sitter highlight examples/sample.as
```

## Development

### Prerequisites

- Node.js (for `tree-sitter-cli`)
- A C compiler (for the parser and external scanner)
- Rust toolchain (for the crate)

### Build and test

```bash
# Install tree-sitter CLI
npm install

# Generate the parser from grammar.js
npx tree-sitter generate

# Run the test corpus (159 tests)
npx tree-sitter test

# Parse an example file (should produce zero ERROR nodes)
npx tree-sitter parse examples/sample.as

# Verify syntax highlighting
npx tree-sitter highlight examples/sample.as

# Build and test the Rust crate
cargo build
cargo test
```

### Project structure

```
grammar.js              # Grammar definition (source of truth)
src/
  scanner.c             # External scanner for template <> disambiguation
  parser.c              # Generated parser (tracked for downstream consumers)
  lib.rs                # Rust crate FFI bindings
  node-types.json       # Generated node type metadata
  tree_sitter/parser.h  # Generated parser header
queries/
  highlights.scm        # Syntax highlighting queries
  tags.scm              # Code navigation queries
test/corpus/            # Test corpus (159 tests)
  basics.txt
  declarations.txt
  expressions.txt
  functions.txt
  statements.txt
  types.txt
examples/
  sample.as             # Comprehensive example for manual testing
```

## License

The tree-sitter-angelscript published under BSD-3-Clause license. For more details read [LICENSE](https://github.com/Relrin/tree-sitter-angelscript/blob/master/LICENSE) file.

