#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/types/ExpressionTypeCache.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves the type name of the first variable or property symbol found in a span of candidates.
 * @param[in] syms Span of matching candidate symbols to inspect.
 * @param[in] table Symbol table containing container type information.
 * @return Type name string of the resolved variable or property, or empty string if none match.
 */
[[nodiscard]] std::string ExtractVariableOrPropertyType(std::span<const Symbol> syms, const SymbolTable& table);

/**
 * @brief Resolves scoped enum member type (e.g. `MyEnum::Member`).
 * @param[in] whole Full scoped identifier text.
 * @param[in] scopeSymbols Symbols resolved from scope lookup for the qualifier.
 * @param[in] symbolTable Symbol table to query.
 * @return Qualified enum type name, or empty string.
 */
[[nodiscard]] std::string ResolveScopedEnumMember(std::string_view whole, std::span<const Symbol> scopeSymbols,
                                                  const SymbolTable& symbolTable);

/**
 * @brief Resolves scoped identifier type from global table, scope symbols, or enum members.
 * @param[in] scopedText Raw or global-prefixed scoped identifier (e.g. `::ns::var` or `Enum::Val`).
 * @param[in] wholeScopeSymbols Scope symbols matching whole identifier name.
 * @param[in] qualScopeSymbols Scope symbols matching the qualifier prefix.
 * @param[in] symbolTable Symbol table to query.
 * @return Resolved type name, or empty string.
 */
[[nodiscard]] std::string ResolveScopedIdentifierType(std::string_view scopedText,
                                                      std::span<const Symbol> wholeScopeSymbols,
                                                      std::span<const Symbol> qualScopeSymbols,
                                                      const SymbolTable& symbolTable);

/**
 * @brief Fallback lookup for global accessor functions (get_X, set_X).
 * @param[in] name Property name without prefix.
 * @param[in] symbolTable Symbol table to query.
 * @return Return or parameter type of the accessor, or empty string.
 */
[[nodiscard]] std::string ResolveIdentifierAccessorFallback(std::string_view name, const SymbolTable& symbolTable);

/**
 * @brief Resolves member symbols or accessors from the enclosing class hierarchy.
 * @param[in] name Identifier name.
 * @param[in] containers Enclosing container declarations.
 * @param[in] symbolTable Symbol table to query.
 * @param[in,out] cache Optional expression type cache.
 * @return Matching member symbols or property accessors, if found.
 */
[[nodiscard]] std::vector<Symbol> ResolveEnclosingContainerMemberSymbols(std::string_view name,
                                                                         std::span<const ContainerInfo> containers,
                                                                         const SymbolTable& symbolTable,
                                                                         ExpressionTypeCache* cache);

/**
 * @brief Resolves identifier type from a host class hierarchy for mixins or virtual documents.
 * @param[in] name Identifier name.
 * @param[in] hostClass Host class name.
 * @param[in] symbolTable Symbol table to query.
 * @param[in,out] cache Optional expression type cache.
 * @return Inferred type name, or empty string.
 */
[[nodiscard]] std::string ResolveIdentifierHostFallbackFromHierarchy(std::string_view name, std::string_view hostClass,
                                                                     const SymbolTable& symbolTable,
                                                                     ExpressionTypeCache* cache);

/**
 * @brief Resolves 'this' expression type given a host class name or enclosing container list.
 * @param[in] hostClass Virtual or mixin host class name.
 * @param[in] containers Enclosing container declarations.
 * @return Cleaned class/interface type name, or empty string.
 */
[[nodiscard]] std::string ResolveThisType(std::string_view hostClass, std::span<const ContainerInfo> containers);

/**
 * @brief Extracts type from resolved candidate symbols (variable, property, or accessor).
 * @param[in] syms Candidate symbols.
 * @param[in] symbolTable Symbol table to query.
 * @return Resolved cleaned type string, or empty string.
 */
[[nodiscard]] std::string ExtractFirstSymbolTypeOrAccessor(std::span<const Symbol> syms,
                                                           const SymbolTable& symbolTable);

} // namespace angel_lsp::analysis
