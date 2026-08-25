# E2E Test Infra: AngelScript LSP Phase 4

## Test Philosophy
- Opaque-box, requirement-driven, and unit/integration layered.
- Methodology: Category-Partition + Boundary Value Analysis (BVA) + Pairwise Combinatorial + Real-World Workload Testing.

## Feature Inventory & Test Mapping
| # | Feature | Requirement | Tier 1 (Coverage) | Tier 2 (Boundary/Edge) | Tier 3 (Cross-Feature) | Tier 4 (Real-World) |
|---|---------|-------------|:-----------------:|:----------------------:|:----------------------:|:-------------------:|
| 1 | Binary/Ternary/Chain Type Deduction | R1.1 | 5 | 5 | ✓ | ✓ |
| 2 | Argument-Based Overload Resolution | R1.2 | 5 | 5 | ✓ | ✓ |
| 3 | Definite Assignment Analysis | R1.3 | 5 | 5 | ✓ | ✓ |
| 4 | Extract Variable Refactoring | R2.1 | 5 | 5 | ✓ | ✓ |
| 5 | Extract Method Refactoring | R2.2 | 5 | 5 | ✓ | ✓ |
| 6 | Getter/Setter Generation | R2.3 | 5 | 5 | ✓ | ✓ |
| 7 | Missing `const` Quick Fix & Intention | R2.4 | 5 | 5 | ✓ | ✓ |
| 8 | Sort & Clean `#include` Action | R2.5 | 5 | 5 | ✓ | ✓ |
| 9 | Built-in Predefined Engine Profiles | R3.1 | 5 | 5 | ✓ | ✓ |
| 10 | Multi-Platform CI/CD & Packaging | R3.2 | 5 | 5 | ✓ | ✓ |
| 11 | VS Code AngelScript Snippets | R3.3 | 5 | 5 | ✓ | ✓ |

## Test Architecture
- Backend Unit & Integration Tests: Doctest framework in `server/tests/` running via `ctest --test-dir server/build -C Debug --output-on-failure`.
- Frontend Extension Tests & Compilation: `npm --prefix client run compile` and `npm --prefix client run lint`.
- Test Case Requirements:
  - In-memory document creation via `test::CreateTestDocument`.
  - In-memory symbol table population via `test::PopulateTestSymbolTable`.
  - Zero disk I/O dependency in pure feature tests.
  - Layer matrix compliance: test files may include Layers 1, 2, and 3.

## Coverage Goals
- Tier 1: ≥5 per feature
- Tier 2: ≥5 per feature (boundary conditions, malformed AST, invalid types)
- Tier 3: Pairwise combinations (e.g. Extract Variable on chained calls with overloaded operators)
- Tier 4: Real-world scripts (game engine entity scripts, Sven Co-op weapon scripts, Urho3D scene components)
