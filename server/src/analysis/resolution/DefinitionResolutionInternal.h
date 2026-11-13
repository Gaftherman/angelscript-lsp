#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/DefinitionResolution.h"
#include "analysis/resolution/TargetResolution.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a symbol type represents a user-defined type definition target.
 * @param[in] type Symbol type enum.
 * @return True if Class, Interface, Enum, Typedef, or Funcdef.
 */
[[nodiscard]] bool IsTypeDefType(SymbolType type) noexcept;

/**
 * @brief Resolves the clean target type name for an identifier expression at cursor.
 * @param[in] nodeText Symbol name under cursor.
 * @param[in] query Definition query context.
 * @return Clean target type name or empty string.
 */
[[nodiscard]] std::string ResolveTargetTypeName(std::string_view nodeText, const DefinitionQuery& query);

/**
 * @brief Filters candidate container symbols using following scoped member reference.
 * @param[in,out] symbols Candidate container symbols.
 * @param[in] innerScope Innermost lexical scope at query position.
 * @param[in] query Definition query context.
 */
void DisambiguateCandidateContainers(std::vector<Symbol>& symbols, const Scope* innerScope,
                                     const DefinitionQuery& query);

/**
 * @brief Resolves class member symbols for a target descriptor with pass-1 call arity matching.
 * @param[in] target Target symbol descriptor.
 * @param[in] innerScope Innermost lexical scope at query position.
 * @param[in] query Definition query context.
 * @return List of matching member symbols.
 */
[[nodiscard]] std::vector<Symbol> ResolveClassMemberSymbols(const TargetDescriptor& target, const Scope* innerScope,
                                                            const DefinitionQuery& query);

/**
 * @brief Attempts to resolve scoped identifier references (e.g. Foo::Bar).
 * @param[in] query Definition query context.
 * @return Resolved scoped member symbols or empty vector.
 */
[[nodiscard]] std::vector<Symbol> TryResolveScopedIdentifier(const DefinitionQuery& query);

/**
 * @brief Attempts to resolve the 'this' keyword to its enclosing class symbol.
 * @param[in] nodeText Identifier text at query position.
 * @param[in] query Definition query context.
 * @return Enclosing class symbols or empty vector.
 */
[[nodiscard]] std::vector<Symbol> TryResolveThisKeyword(std::string_view nodeText, const DefinitionQuery& query);

/**
 * @brief Resolves base class delegate property symbols (e.g. BaseClass).
 * @param[in] nodeText Identifier text at query position.
 * @param[in] query Definition query context.
 * @return Base class definition symbols or empty vector.
 */
[[nodiscard]] std::vector<Symbol> TryResolveDelegateProperty(std::string_view nodeText, const DefinitionQuery& query);

} // namespace angel_lsp::analysis
