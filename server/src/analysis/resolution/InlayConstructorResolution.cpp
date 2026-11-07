#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/InlayCalleeResolution.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Matches the best constructor overload using argument types.
 * @param[in] candidateSymbols Candidate constructor symbols.
 * @param[in] declaratorNode AST variable_declarator or construct_call node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] args Parsed argument information.
 * @return Pointer to best matching symbol, or nullptr.
 */
static const Symbol* MatchConstructorOverload(const std::vector<Symbol>& candidateSymbols, TSNode declaratorNode,
                                              const ExpressionTypeContext& ctx,
                                              std::span<const IndexedCallArgument> args)
{
    if (candidateSymbols.size() <= 1)
    {
        return nullptr;
    }

    const Scope* rootScope = ctx.scope;
    while (rootScope && rootScope->parent)
    {
        rootScope = rootScope->parent;
    }
    TSPoint pt = ts_node_start_point(declaratorNode);
    const Scope* scope = rootScope ? FindInnermostScope(rootScope, pt.row, pt.column) : nullptr;

    std::vector<std::string> argTypes;
    argTypes.reserve(args.size());
    ExpressionTypeContext argCtx{scope, ctx.symbolTable, ctx.sourceCode, ctx.uri};
    argCtx.nodeIndex = ctx.nodeIndex;
    argCtx.stringTypeName = ctx.stringTypeName;
    argCtx.arrayTypeName = ctx.arrayTypeName;
    for (const auto& arg : args)
    {
        TSNode argNode = RecoverNodeAt(ctx, arg.startByte, arg.endByte);
        std::string aType = !ts_node_is_null(argNode) ? ResolveExpressionType(argNode, argCtx) : "";
        argTypes.push_back(std::move(aType));
    }

    auto match = ResolveBestOverload(candidateSymbols, argTypes, ctx.symbolTable);
    return match.bestCandidate;
}

ResolvedCallee ResolveConstructorParameters(std::string_view declaredTypeName, TSNode declaratorNode,
                                            const ExpressionTypeContext& ctx, std::span<const IndexedCallArgument> args)
{
    std::string baseName = CleanBaseType(std::string(declaredTypeName));
    if (baseName.empty())
    {
        return {};
    }

    std::vector<Symbol> candidateSymbols =
        CollectConstructorCandidates(baseName, declaratorNode, ctx.sourceCode, ctx.symbolTable);
    if (candidateSymbols.empty())
    {
        return {};
    }

    const Symbol* bestSym = MatchConstructorOverload(candidateSymbols, declaratorNode, ctx, args);

    if (!bestSym)
    {
        bestSym = SelectCandidateByArgCount(candidateSymbols, args.size());
    }

    if (!bestSym)
    {
        for (const auto& sym : candidateSymbols)
        {
            if (sym.type == SymbolType::Function)
            {
                bestSym = &sym;
                break;
            }
        }
    }

    if (bestSym && bestSym->type == SymbolType::Function)
    {
        return ExtractCalleeFromSymbol(bestSym);
    }

    return {};
}

std::optional<ResolvedCallee> ResolveConstructorInitCallee(const IndexedConstructorInitDeclarator& decl,
                                                           std::span<const IndexedCallArgument> args,
                                                           const ExpressionTypeContext& ctx)
{
    TSNode declarator = RecoverNodeAt(ctx, decl.startByte, decl.endByte);
    if (ts_node_is_null(declarator))
    {
        return std::nullopt;
    }
    ResolvedCallee callee = ResolveConstructorParameters(decl.declaredTypeName, declarator, ctx, args);
    if (callee.calleeName.empty() && callee.parameters.empty())
    {
        return std::nullopt;
    }
    return callee;
}

} // namespace angel_lsp::analysis
