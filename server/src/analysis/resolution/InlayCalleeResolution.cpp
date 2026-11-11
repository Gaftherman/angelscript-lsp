#include "analysis/resolution/InlayCalleeResolution.h"

#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/AstRecoveryInternal.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/CallableResolutionInternal.h"
#include "analysis/resolution/InlayCalleeResolutionInternal.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/ScopeLookupInternal.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Locates the function expression child node for a call_expression.
 * @param[in] callNode AST call_expression node.
 * @return Function node, or null node if not found.
 */
static TSNode FindCalleeFunctionNode(TSNode callNode)
{
    TSNode funcNode = parser::GetChildByField(callNode, parser::GetGrammarSymbols().fieldFunction);
    if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
    {
        funcNode = ts_node_child(callNode, 0);
    }
    return funcNode;
}

/**
 * @brief Collects constructor candidates for class symbols using fully qualified names with fallback.
 * @param[in] candidateSymbols Symbols found in scope or table.
 * @param[in] symbolTable Symbol table for resolution.
 * @return Vector of constructor function symbols.
 */
static std::vector<Symbol> CollectClassConstructorCandidates(const std::vector<Symbol>& candidateSymbols,
                                                             const SymbolTable& symbolTable)
{
    std::vector<Symbol> ctorSymbols;
    for (const auto& sym : candidateSymbols)
    {
        if (sym.type == SymbolType::Class)
        {
            const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            auto ctors = symbolTable.FindMemberSymbols(qName, sym.name);
            if (ctors.empty() && qName != sym.name)
            {
                ctors = symbolTable.FindMemberSymbols(sym.name, sym.name);
            }
            ctorSymbols.insert(ctorSymbols.end(), ctors.begin(), ctors.end());
        }
    }
    return ctorSymbols;
}

/**
 * @brief Collects candidate function symbols for a free or unqualified call expression.
 * @param[in] funcNode AST callee function node.
 * @param[in] callNode AST call_expression node.
 * @param[in] ctx Semantic expression type context.
 * @return Vector of candidate symbols.
 */
static std::vector<Symbol> CollectFreeCalleeCandidates(TSNode funcNode, TSNode callNode,
                                                       const ExpressionTypeContext& ctx)
{
    std::string calleeName =
        std::string(utils::StripGlobalPrefix(std::string(parser::NodeText(funcNode, ctx.sourceCode))));
    auto candidateSymbols = FindSymbolsInScope(calleeName, callNode, ctx.sourceCode, ctx.symbolTable);

    CollectEnclosingClassCallees(calleeName, callNode, ctx, candidateSymbols);

    if (candidateSymbols.empty())
    {
        candidateSymbols = ctx.symbolTable.FindSymbols(calleeName);
    }

    if (candidateSymbols.empty())
    {
        candidateSymbols = CollectScopedCalleeCandidates(calleeName, callNode, ctx);
    }

    auto ctorSymbols = CollectClassConstructorCandidates(candidateSymbols, ctx.symbolTable);
    if (!ctorSymbols.empty())
    {
        return ctorSymbols;
    }

    if (candidateSymbols.empty())
    {
        auto fallbackCtors = CollectConstructorCandidates(calleeName, callNode, ctx.sourceCode, ctx.symbolTable);
        if (!fallbackCtors.empty())
        {
            return fallbackCtors;
        }
    }

    if (candidateSymbols.empty())
    {
        const Scope* rootScope = ctx.scope;
        while (rootScope && rootScope->parent)
        {
            rootScope = rootScope->parent;
        }
        if (rootScope)
        {
            TSPoint pt = ts_node_start_point(callNode);
            if (const auto* scope = FindInnermostScope(rootScope, pt.row, pt.column))
            {
                if (auto callableSym = TryResolveCallableFuncdef(calleeName, scope, ctx.symbolTable))
                {
                    candidateSymbols.push_back(std::move(*callableSym));
                }
            }
        }
    }

    return candidateSymbols;
}

/**
 * @brief Resolves parameters for a function, method, or constructor called by a call_expression node.
 * @param[in] callNode AST call_expression node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] numArgs Number of arguments passed in the call.
 * @return Callee resolution result matching the call.
 */
static ResolvedCallee ResolveCalleeParameters(TSNode callNode, const ExpressionTypeContext& ctx, size_t numArgs)
{
    TSNode funcNode = FindCalleeFunctionNode(callNode);
    if (ts_node_is_null(funcNode))
    {
        return {};
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol funcType = ts_node_symbol(funcNode);
    std::vector<Symbol> candidateSymbols = (funcType == gs.symMemberExpression)
                                               ? CollectMemberCalleeCandidates(funcNode, ctx)
                                               : CollectFreeCalleeCandidates(funcNode, callNode, ctx);

    const Symbol* bestSym = nullptr;
    if (candidateSymbols.size() > 1)
    {
        const Scope* rootScope = ctx.scope;
        while (rootScope && rootScope->parent)
        {
            rootScope = rootScope->parent;
        }
        const Scope* scope = nullptr;
        if (rootScope)
        {
            TSPoint pt = ts_node_start_point(callNode);
            scope = FindInnermostScope(rootScope, pt.row, pt.column);
        }
        ExpressionTypeContext callArgCtx{scope, ctx.symbolTable, ctx.sourceCode, ctx.uri};
        callArgCtx.nodeIndex = ctx.nodeIndex;
        callArgCtx.stringTypeName = ctx.stringTypeName;
        callArgCtx.arrayTypeName = ctx.arrayTypeName;
        auto argTypes = ExtractCallArgumentTypes(callNode, callArgCtx);
        auto match = ResolveBestOverload(candidateSymbols, argTypes, ctx.symbolTable);
        if (match.bestCandidate != nullptr)
        {
            bestSym = match.bestCandidate;
        }
    }

    if (!bestSym)
    {
        bestSym = SelectCandidateByArgCount(candidateSymbols, numArgs);
    }
    if (!bestSym && !candidateSymbols.empty())
    {
        bestSym = SelectFallbackCandidate(candidateSymbols);
    }

    return ExtractCalleeFromSymbol(bestSym);
}

std::optional<ResolvedCallee> ResolveCallSiteCallee(const IndexedCallSite& site,
                                                    std::span<const IndexedCallArgument> args,
                                                    const ExpressionTypeContext& ctx)
{
    TSNode callNode = RecoverNodeAt(ctx, site.startByte, site.endByte);
    if (ts_node_is_null(callNode))
    {
        return std::nullopt;
    }
    if (site.isConstructorCall)
    {
        std::string_view typeText = site.constructTypeName;
        if (typeText.empty())
        {
            TSNode typeNode = parser::GetChildByField(callNode, parser::GetGrammarSymbols().fieldType);
            if (!ts_node_is_null(typeNode))
            {
                typeText = parser::NodeText(typeNode, ctx.sourceCode);
            }
        }
        if (typeText.empty())
        {
            return std::nullopt;
        }
        ResolvedCallee callee = ResolveConstructorParameters(typeText, site.startByte, ctx, args);
        if (callee.calleeName.empty() && callee.parameters.empty())
        {
            return std::nullopt;
        }
        return callee;
    }

    ResolvedCallee callee = ResolveCalleeParameters(callNode, ctx, args.size());
    if (callee.calleeName.empty() && callee.parameters.empty())
    {
        return std::nullopt;
    }
    return callee;
}

} // namespace angel_lsp::analysis
