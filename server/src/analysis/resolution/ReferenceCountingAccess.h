#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ReferenceCounting.h"

#include <span>
#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Identifies constructor declarations from their owning class and name.
 * @param[in] symbol Declaration to classify.
 * @return True for a constructor belonging to a class.
 */
[[nodiscard]] bool IsConstructorDeclaration(const Symbol& symbol);

/**
 * @brief Finds the bare variables initialized by a single written type reference.
 * @param[in] typeReference Reference recording the type identifier's byte position.
 * @param[in] scope Lexical scope sharing the document's index.
 * @return Borrowed declarator sites, empty when the type reference has no bare declarators.
 */
[[nodiscard]] std::span<const IndexedDefaultConstruction>
FindDefaultConstructionSites(const LocalReference& typeReference, const Scope& scope);

/**
 * @brief Symbol snapshot and analyzed engine policy used to select a default constructor.
 */
struct DefaultConstructorLookup
{
    const SymbolTable& symbolTable;        ///< Declarations available to resolve the written type.
    bool alwaysImplementsDefaultConstruct; ///< Whether the engine supplies a parameterless script constructor.
};

/**
 * @brief Finds the bare declarators that select a specific constructor.
 * @param[in] typeReference Written type reference for the declaration.
 * @param[in] scope Lexical scope used to resolve the type and constructor access.
 * @param[in] constructor Canonical constructor declaration under search.
 * @param[in] lookup Symbol snapshot and default constructor policy of the document containing the use.
 * @return Borrowed use sites, empty if the reference does not select the constructor.
 */
[[nodiscard]] std::span<const IndexedDefaultConstruction>
FindDefaultConstructorUses(const LocalReference& typeReference, const Scope& scope, const Symbol& constructor,
                           const DefaultConstructorLookup& lookup);

/**
 * @brief Determines if a reference matches a known declaration or definition site.
 * @param[in] fileUri URI of document owning scope.
 * @param[in] ref Reference to check.
 * @param[in] scope Lexical scope of the reference.
 * @param[in] criteria Active search criteria.
 * @return True if reference represents a declaration or definition site.
 */
[[nodiscard]] bool IsDeclarationOrDefinition(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                             const ReferenceCollectionCriteria& criteria);

/**
 * @brief Checks whether a reference has valid accessibility and scope visibility.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if access is valid.
 */
[[nodiscard]] bool IsValidAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                 const ReferenceCollectionCriteria& criteria);
} // namespace angel_lsp::analysis
