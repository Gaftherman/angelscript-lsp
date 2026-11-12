#include "analysis/index/ExpressionTypeASTInternal.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeCall.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves member method call expression type.
 * @param[in] funcNode Function AST node (member_expression).
 * @param[in] argTypes Evaluated argument types.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved return type or empty string.
 */
static std::string ResolveMemberCallExpr(TSNode funcNode, const std::vector<std::string>& argTypes,
                                         const ExpressionTypeContext& ctx)
{
    const SyntaxOperands syntax(funcNode, ctx.nodeIndex);
    const TSNode objNode = syntax.Field(parser::GetGrammarSymbols().fieldObject);
    const TSNode memNode = syntax.Field(parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }

    std::string objType = ResolveASTExpressionType(objNode, ctx);
    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);
    std::string fallbackObj = GetTrimmedNodeText(objNode, ctx.sourceCode);

    return ResolveMemberCallReturn(MemberCallResolutionRequest{objType, memName, fallbackObj, ctx.arrayTypeName},
                                   argTypes, ctx.symbolTable);
}

/**
 * @brief Resolves non-member or bare function calls.
 * @param[in] exprNode Call expression AST node.
 * @param[in] funcNode Callee AST node.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved return type or empty string.
 */
static std::string ResolveBareCallExpr(TSNode exprNode, TSNode funcNode, const std::vector<std::string>& argTypes,
                                       const ExpressionTypeContext& ctx)
{
    std::string funcName = GetTrimmedNodeText(funcNode, ctx.sourceCode);
    if (argTypes.size() == 1 && IsCorePrimitive(funcName))
    {
        return funcName;
    }
    auto containers = ResolveEnclosingContainers(exprNode, ctx.sourceCode, ctx.nodeIndex);
    std::vector<Symbol> candidates = CollectEnclosingCallCandidates(containers, funcName, ctx.symbolTable, ctx.cache);
    if (candidates.empty())
    {
        std::vector<Symbol> inScope =
            ResolveScopeSymbols(funcName, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        CollectScopeAndGlobalFunctions(funcName, inScope, ctx.symbolTable, candidates);
    }
    if (!candidates.empty())
    {
        return ResolveCallOverloadReturn(candidates, argTypes, ctx.symbolTable);
    }
    return FindClassOrInterfaceName(funcName, ctx.symbolTable);
}

std::string ResolveASTCallExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode funcNode = syntax.Callee();
    if (ts_node_is_null(funcNode))
    {
        return "";
    }

    std::vector<std::string> argTypes;
    const TSNode argsNode = syntax.Field(gs.fieldArguments);
    SyntaxOperands(argsNode, ctx.nodeIndex)
        .ForEachNamed(
            [&](TSNode arg)
            {
                argTypes.push_back(ResolveASTExpressionType(arg, ctx));
                return true;
            });

    if (ts_node_symbol(funcNode) == gs.symMemberExpression)
    {
        if (auto res = ResolveMemberCallExpr(funcNode, argTypes, ctx); !res.empty())
        {
            return res;
        }
    }
    else
    {
        if (auto res = ResolveBareCallExpr(exprNode, funcNode, argTypes, ctx); !res.empty())
        {
            return res;
        }
    }

    std::string calleeType = ResolveASTExpressionType(funcNode, ctx);
    return ResolveCalleeFuncdefReturn(calleeType, ctx.symbolTable);
}

} // namespace angel_lsp::analysis
