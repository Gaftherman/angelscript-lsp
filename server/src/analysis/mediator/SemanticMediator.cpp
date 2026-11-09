#include "analysis/mediator/SemanticMediator.h"

namespace angel_lsp::analysis
{

SemanticMediator::SemanticMediator(const SymbolTable& st, const Scope* sc, const NodeIndex* ni, std::string_view src,
                                   std::string_view uri, ExpressionTypeCache* cache,
                                   const config::EngineRuleConfig* rules)
    : m_symbolTable(st), m_scopeRoot(sc), m_nodeIndex(ni), m_sourceCode(src), m_fileUri(uri), m_cache(cache),
      m_engineRules(rules)
{
}

const Scope* SemanticMediator::ScopeAtPoint(uint32_t line, uint32_t col) const noexcept
{
    return m_scopeRoot ? FindInnermostScope(m_scopeRoot, line, col) : nullptr;
}

} // namespace angel_lsp::analysis
