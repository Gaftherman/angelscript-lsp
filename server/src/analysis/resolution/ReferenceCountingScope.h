#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ReferenceCounting.h"

#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Finds the innermost class or interface enclosing a line, filling the per-file span cache on first use.
 * @param[in] symbolTable Global symbol table.
 * @param[in] uri Document file URI.
 * @param[in] line Document line.
 * @param[in,out] cache Per-file class span cache that owns the returned name.
 * @return View into cache of the enclosing class name, valid while cache lives; empty if none.
 */
[[nodiscard]] std::string_view GetEnclosingClassName(const SymbolTable& symbolTable, std::string_view uri,
                                                     uint32_t line, EnclosingClassCache& cache);

/**
 * @brief Checks if a container symbol is a class or interface rather than a namespace.
 * @param[in] container Name of the container.
 * @param[in] symbolTable Global symbol table.
 * @return True if container is a class or interface.
 */
[[nodiscard]] bool IsContainerClass(std::string_view container, const SymbolTable& symbolTable);

/**
 * @brief Checks if a symbol belongs to a class or interface container.
 * @param[in] sym Target symbol.
 * @param[in] symbolTable Global symbol table.
 * @return True if container is a class or interface.
 */
[[nodiscard]] bool IsSymbolInClassContainer(const Symbol& sym, const SymbolTable& symbolTable);

/**
 * @brief Checks whether a reference at given line in fileUri is inside a named namespace.
 * @param[in] fileUri Document URI.
 * @param[in] line Reference line.
 * @param[in] nsName Namespace name.
 * @param[in] symbolTable Global symbol table.
 * @param[in,out] classCache Per-file class span cache.
 * @return True if inside namespace block.
 */
[[nodiscard]] bool IsInsideNamespace(std::string_view fileUri, uint32_t line, std::string_view nsName,
                                     const SymbolTable& symbolTable, EnclosingClassCache& classCache);

/**
 * @brief Checks if a using-namespace directive for targetNs is active at refByte.
 * @param[in] scope Lexical scope containing the reference.
 * @param[in] refByte Start byte offset of the reference.
 * @param[in] targetNs Target namespace name.
 * @return True if an enclosing using-namespace directive imports targetNs.
 */
[[nodiscard]] bool IsUsingNamespaceActive(const Scope* scope, uint32_t refByte, std::string_view targetNs) noexcept;

/**
 * @brief Checks if a type qualifier matches any compatible class or active using-namespace class.
 * @param[in] qualifier Extracted scope qualifier string.
 * @param[in] scope Lexical scope containing the reference.
 * @param[in] refByte Start byte offset of the reference.
 * @param[in] criteria Active reference collection criteria.
 * @return True if qualifier resolves to one of criteria.compatibleClasses.
 */
[[nodiscard]] bool MatchesClassTypeQualifier(std::string_view qualifier, const Scope* scope, uint32_t refByte,
                                             const ReferenceCollectionCriteria& criteria);

/**
 * @brief Finds the nearest enclosing class or namespace that declares a type.
 * @param[in] scope Lexical scope of the unqualified use.
 * @param[in] name Unqualified type name.
 * @param[in] table Symbol table used for type lookup.
 * @return Container name borrowed from the scope, or empty if no enclosing container declares the type.
 */
[[nodiscard]] std::string_view FindEnclosingTypeContainer(const Scope& scope, std::string_view name,
                                                          const SymbolTable& table);

/**
 * @brief Checks which container owns a written class type reference.
 * @param[in] reference Indexed type occurrence, including its authored qualifier.
 * @param[in] scope Lexical scope at the occurrence.
 * @param[in] container Container of the declaration under search, empty for global types.
 * @param[in] table Symbol table used for lexical shadowing.
 * @return True when the occurrence names the type from the given container.
 */
[[nodiscard]] bool MatchesWrittenTypeOwner(const LocalReference& reference, const Scope& scope,
                                           std::string_view container, const SymbolTable& table);

/**
 * @brief Checks if a global type target is shadowed by an enclosing class or namespace type.
 * @param[in] scope Lexical scope containing the reference.
 * @param[in] criteria Active reference collection criteria.
 * @return True if shadowed by a homonymous type in an enclosing container.
 */
[[nodiscard]] bool IsGlobalTypeShadowed(const Scope* scope, const ReferenceCollectionCriteria& criteria);
} // namespace angel_lsp::analysis
