#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "config/EngineRuleConfig.h"

#include <cstdint>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Central mediator coordinating AST index, scopes, symbols, and resolution passes.
 * @note Holds non-owning views only; every bound structure must outlive the mediator.
 */
class SemanticMediator
{
  public:
    /**
     * @brief Constructs a mediator binding symbol table, scopes, node index, and caches.
     * @param[in] st Active symbol table.
     * @param[in] sc Document scope tree root.
     * @param[in] ni Pre-computed AST node index.
     * @param[in] src Document source buffer.
     * @param[in] uri Document file URI.
     * @param[in] cache Optional expression type memoization cache.
     * @param[in] rules Optional engine rule configuration.
     */
    SemanticMediator(const SymbolTable& st, const Scope* sc, const NodeIndex* ni, std::string_view src,
                     std::string_view uri = "", ExpressionTypeCache* cache = nullptr,
                     const config::EngineRuleConfig* rules = nullptr);

    ~SemanticMediator() = default;
    SemanticMediator(const SemanticMediator&) = delete;
    SemanticMediator& operator=(const SemanticMediator&) = delete;
    SemanticMediator(SemanticMediator&&) noexcept = default;
    SemanticMediator& operator=(SemanticMediator&&) noexcept = default;

    /**
     * @brief Finds the innermost lexical scope enclosing a zero-based source position.
     * @param[in] line Zero-based line of the position.
     * @param[in] col Zero-based byte column of the position.
     * @return Innermost enclosing scope, or nullptr when no scope tree is bound.
     */
    [[nodiscard]] const Scope* ScopeAtPoint(uint32_t line, uint32_t col) const noexcept;

    /**
     * @brief Returns the symbol table every query resolves against.
     * @return Bound symbol table.
     */
    [[nodiscard]] const SymbolTable& GetSymbolTable() const noexcept
    {
        return m_symbolTable;
    }

    /**
     * @brief Returns the root of the document scope tree.
     * @return Scope tree root, or nullptr when the document has none.
     */
    [[nodiscard]] const Scope* GetScopeRoot() const noexcept
    {
        return m_scopeRoot;
    }

    /**
     * @brief Returns the first-pass node index of the document.
     * @return Node index, or nullptr when analysis runs without one.
     */
    [[nodiscard]] const NodeIndex* GetNodeIndex() const noexcept
    {
        return m_nodeIndex;
    }

    /**
     * @brief Returns the analysed document text.
     * @return View of the source buffer owned by the caller.
     */
    [[nodiscard]] std::string_view GetSourceCode() const noexcept
    {
        return m_sourceCode;
    }

    /**
     * @brief Returns the URI of the analysed document.
     * @return View of the URI owned by the caller; empty when none was supplied.
     */
    [[nodiscard]] std::string_view GetFileUri() const noexcept
    {
        return m_fileUri;
    }

    /**
     * @brief Returns the expression type memoization cache shared by the checkers.
     * @return Cache, or nullptr when memoization is disabled.
     */
    [[nodiscard]] ExpressionTypeCache* GetCache() const noexcept
    {
        return m_cache;
    }

    /**
     * @brief Returns the engine rule configuration active for this analysis.
     * @return Rule configuration, or nullptr when no engine rules apply.
     */
    [[nodiscard]] const config::EngineRuleConfig* GetEngineRules() const noexcept
    {
        return m_engineRules;
    }

  private:
    const SymbolTable& m_symbolTable;
    const Scope* m_scopeRoot = nullptr;
    const NodeIndex* m_nodeIndex = nullptr;
    std::string_view m_sourceCode;
    std::string_view m_fileUri;
    ExpressionTypeCache* m_cache = nullptr;
    const config::EngineRuleConfig* m_engineRules = nullptr;
};

} // namespace angel_lsp::analysis
