#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/Symbol.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/TargetResolution.h"

#include <memory>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Categorizes symbol occurrences into semantic read, write, or declaration text access.
 */
enum class OccurrenceAccess : uint8_t
{
    Text,  ///< Occurrence is in a declaration, type reference, or namespace.
    Read,  ///< Occurrence is read or invoked without mutation.
    Write, ///< Occurrence is written, incremented/decremented, or passed as a mutable out/inout reference.
};

/**
 * @brief Classifies symbol occurrences into Text, Read, or Write access kinds.
 */
class OccurrenceAccessClassifier
{
  public:
    /**
     * @brief Constructs an occurrence access classifier for the specified document context.
     * @param[in] uri Document file URI.
     * @param[in] sourceCode Source text content.
     * @param[in] symbolTable Workspace symbol table.
     * @param[in] scopeIndex Index of lexical scopes.
     */
    OccurrenceAccessClassifier(std::string_view uri, std::string_view sourceCode, const SymbolTable& symbolTable,
                               const ScopeIndex& scopeIndex);

    /**
     * @brief Classifies the access kind of a target symbol occurrence at the given coordinate range.
     * @param[in] target Target symbol descriptor.
     * @param[in] range Occurrence coordinate range in source code.
     * @return OccurrenceAccess classification (Text, Read, or Write).
     */
    [[nodiscard]] OccurrenceAccess Classify(const TargetDescriptor& target, const SourceRange& range) const;

  private:
    /**
     * @brief Tests whether the callee parameter bound to an argument can modify the caller's storage.
     * @param[in] argument Indexed argument fact linking the argument span to its callee and receiver spans.
     * @param[in] scope Innermost scope of the occurrence, used to type a local receiver; may be null.
     * @return True when a function overload named by the callee declares `&out`, `&inout` or a non-const reference
     *         at the argument index.
     * @note The receiver type is a view into a scope definition or a symbol held by a handle local to this call,
     *       so it never outlives its owner.
     */
    [[nodiscard]] bool IsMutatingArgument(const IndexedCallArgumentRef& argument, const Scope* scope) const;

    std::string_view m_uri;
    std::string_view m_sourceCode;
    const SymbolTable& m_symbolTable;
    std::shared_ptr<const Scope> m_rootScope;
};

} // namespace angel_lsp::analysis
