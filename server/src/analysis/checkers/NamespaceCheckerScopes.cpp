#include "analysis/checkers/NamespaceCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "utils/ScopedNameUtils.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if any symbol or container in the symbol table matches the scope prefix.
 */
static bool HasContainerMatchingPrefix(const SymbolTable& table, std::string_view prefix)
{
    if (const auto ruleIndex = table.GetRuleIndex())
    {
        if (ruleIndex->byContainer.contains(prefix))
        {
            return true;
        }
        for (const auto& [container, _] : ruleIndex->byContainer)
        {
            if (utils::HasScopePrefix(container, prefix))
            {
                return true;
            }
        }
        return false;
    }
    return table.HasSymbol(prefix) || table.HasSymbolAnywhere(prefix);
}

/**
 * @brief Resolves whether a scope qualifier prefix refers to a known namespace, class, or imported scope.
 */
static bool IsKnownScopePrefix(std::string_view prefix, uint32_t byteOffset, const ScopedIdentifierCheckContext& sctx,
                               std::span<const std::string> usings)
{
    if (prefix.empty() || sctx.table.HasSymbol(prefix) || sctx.table.HasSymbolAnywhere(prefix))
    {
        return true;
    }

    if (HasContainerMatchingPrefix(sctx.table, prefix))
    {
        return true;
    }

    if (sctx.nodeIndex)
    {
        const auto containers = sctx.nodeIndex->CollectEnclosingContainers(byteOffset, sctx.sourceCode);
        for (const auto& c : containers)
        {
            const std::string q = utils::JoinScopedName(c.qualifiedName, prefix);
            if (sctx.table.HasSymbol(q) || sctx.table.HasSymbolAnywhere(q))
            {
                return true;
            }
        }
    }

    for (const auto& ns : usings)
    {
        const std::string q = utils::JoinScopedName(ns, prefix);
        if (sctx.table.HasSymbol(q) || sctx.table.HasSymbolAnywhere(q))
        {
            return true;
        }
    }

    return false;
}

void CheckScopedIdentifier(const ScopedIdentifierFact& fact, const ScopedIdentifierCheckContext& sctx,
                           DiagnosticContext& ctx)
{
    if (fact.prefix.empty())
    {
        return;
    }

    const auto usings = sctx.nodeIndex ? sctx.nodeIndex->CollectUsingNamespaces(sctx.sourceCode, fact.bytes.startByte)
                                       : GetUsingNamespaces(ctx.request);
    if (!IsKnownScopePrefix(fact.prefix, fact.bytes.startByte, sctx, usings))
    {
        ctx.EmitAtRange(fact.prefixRange, "as-err-undefined-namespace", fact.prefix, DiagnosticSeverity::Error);
    }
}

void CheckImportBody(const ImportDeclarationFact& fact, DiagnosticContext& ctx)
{
    if (fact.hasBodyBlock)
    {
        ctx.EmitAtRange(fact.range, "as-err-import-has-body", "import", DiagnosticSeverity::Error);
    }
}

} // namespace angel_lsp::analysis
