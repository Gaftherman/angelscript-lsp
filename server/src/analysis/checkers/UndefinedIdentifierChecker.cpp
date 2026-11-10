#include "analysis/checkers/UndefinedIdentifierChecker.h"

#include "analysis/checkers/UndefinedIdentifierCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"

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
    const std::vector<std::string> usingNamespaces =
        ctx.request.nodeIndex
            ? CollectUsingNamespaces(*ctx.request.nodeIndex, ctx.request.sourceCode)
            : (ctx.request.tree ? CollectUsingNamespaces(ts_tree_root_node(ctx.request.tree), ctx.request.sourceCode)
                                : std::vector<std::string>{});
    const UndefinedIdentifierContext uCtx{knownGlobalNames, mixinRanges, usingNamespaces, ctx};
    CheckUndefinedIdentifiersIterative(scope, uCtx);
}

} // namespace angel_lsp::analysis
