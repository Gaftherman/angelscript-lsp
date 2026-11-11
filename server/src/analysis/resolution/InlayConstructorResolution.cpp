#include "analysis/index/NodeRecoveryInternal.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
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
static const Symbol* MatchConstructorOverload(const std::vector<Symbol>& candidateSymbols,
                                              uint32_t declaratorByteOffset, const ExpressionTypeContext& ctx,
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
    const Scope* scope = ctx.scope ? ctx.scope : rootScope;

    std::vector<std::string> argTypes;
    argTypes.reserve(args.size());
    ExpressionTypeContext argCtx{scope, ctx.symbolTable, ctx.sourceCode, ctx.uri};
    argCtx.nodeIndex = ctx.nodeIndex;
    argCtx.stringTypeName = ctx.stringTypeName;
    argCtx.arrayTypeName = ctx.arrayTypeName;
    for (const auto& arg : args)
    {
        std::string aType;
        if (ctx.nodeIndex)
        {
            TSNode argNode = RecoverNodeAt(*ctx.nodeIndex, arg.startByte, arg.endByte);
            if (!ts_node_is_null(argNode))
            {
                aType = ResolveExpressionType(argNode, argCtx);
            }
        }
        argTypes.push_back(std::move(aType));
    }

    auto match = ResolveBestOverload(candidateSymbols, argTypes, ctx.symbolTable);
    return match.bestCandidate;
}

ResolvedCallee ResolveConstructorParameters(std::string_view declaredTypeName, uint32_t declaratorByteOffset,
                                            const ExpressionTypeContext& ctx,
                                            std::span<const IndexedCallArgument> args)
{
    std::string baseName = CleanBaseType(std::string(declaredTypeName));
    if (baseName.empty())
    {
        return {};
    }

    std::vector<Symbol> candidateSymbols =
        CollectConstructorCandidates(baseName, declaratorByteOffset, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
    if (candidateSymbols.empty())
    {
        return {};
    }

    const Symbol* bestSym = MatchConstructorOverload(candidateSymbols, declaratorByteOffset, ctx, args);

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

ResolvedCallee ResolveConstructorParameters(std::string_view declaredTypeName, TSNode declaratorNode,
                                            const ExpressionTypeContext& ctx,
                                            std::span<const IndexedCallArgument> args)
{
    const uint32_t byteOffset = !ts_node_is_null(declaratorNode) ? ts_node_start_byte(declaratorNode) : 0;
    return ResolveConstructorParameters(declaredTypeName, byteOffset, ctx, args);
}

std::optional<ResolvedCallee> ResolveConstructorInitCallee(const IndexedConstructorInitDeclarator& decl,
                                                           std::span<const IndexedCallArgument> args,
                                                           const ExpressionTypeContext& ctx)
{
    ResolvedCallee callee = ResolveConstructorParameters(decl.declaredTypeName, decl.startByte, ctx, args);
    if (callee.calleeName.empty() && callee.parameters.empty())
    {
        return std::nullopt;
    }
    return callee;
}

} // namespace angel_lsp::analysis
