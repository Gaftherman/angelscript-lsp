#include "analysis/checkers/UndefinedIdentifierChecker.h"

#include "analysis/checkers/UndefinedIdentifierCheckerCommon.h"
#include "analysis/resolution/ScopeSymbolLookup.h"

#include <span>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

void UndefinedIdentifierChecker::Run(const AstCheckContext& ast, DiagnosticContext& ctx)
{
    if (ast.scopeRoot)
    {
        CheckUndefinedIdentifiers(ast.scopeRoot, ctx.request.GetRuleIndex().globalNames, ctx);
    }
}

void UndefinedIdentifierChecker::CheckUndefinedIdentifiers(
    const Scope* scope,
    const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>& knownGlobalNames,
    DiagnosticContext& ctx)
{
    if (!scope)
    {
        return;
    }

    const MixinRanges mixinRanges = CollectMixinRanges(ctx.request.symbolTable, ctx.request.fileUri);
    const auto& usingNamespaces = GetUsingNamespaces(ctx.request);
    const UndefinedIdentifierContext uCtx{knownGlobalNames, mixinRanges, usingNamespaces, ctx};
    CheckUndefinedIdentifiersIterative(scope, uCtx);
}

} // namespace angel_lsp::analysis
