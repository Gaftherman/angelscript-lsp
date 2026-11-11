#include "analysis/resolution/CallableResolutionInternal.h"

#include "analysis/index/CallArgumentFacts.h"
#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/AstRecoveryInternal.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsConstArgumentNode(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (ts_node_is_null(exprNode))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(exprNode);
    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return false;
    }
    const std::string name(utils::TrimView(parser::NodeText(exprNode, ctx.sourceCode)));
    if (ctx.scope)
    {
        if (const auto* def = ResolveInScopeAt(ctx.scope, name, ts_node_start_byte(exprNode)))
        {
            if (!def->typeName.empty() && HasConstModifier(def->typeName))
            {
                return true;
            }
        }
    }
    if (const auto syms = ctx.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& foundSym : *syms)
        {
            if ((foundSym.type == SymbolType::Variable || foundSym.type == SymbolType::Property) &&
                foundSym.GetVariable().modifiers.isConst)
            {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> ExtractCallArgumentTypes(TSNode callNode, const ExpressionTypeContext& ctx)
{
    auto args = ExtractCallArguments(callNode, ctx.sourceCode);
    std::vector<std::string> types;
    types.reserve(args.size());
    for (const auto& arg : args)
    {
        if (ts_node_is_null(arg.exprNode))
        {
            types.push_back("");
        }
        else
        {
            std::string argType = ResolveExpressionType(arg.exprNode, ctx);
            if (!argType.empty() && !HasConstModifier(argType) && IsConstArgumentNode(arg.exprNode, ctx))
            {
                argType.insert(0, "const ");
            }
            types.push_back(std::move(argType));
        }
    }
    return types;
}

std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, TSNode contextNode,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable)
{
    const uint32_t byteOffset = !ts_node_is_null(contextNode) ? ts_node_start_byte(contextNode) : 0;
    return CollectConstructorCandidates(baseName, byteOffset, sourceCode, symbolTable, nullptr);
}

std::optional<Symbol> FuncdefTargetOfLambda(TSNode lambdaNode, const SymbolTable& table, std::string_view sourceCode)
{
    if (!IsLambdaExpression(lambdaNode))
    {
        return std::nullopt;
    }
    return FuncdefTargetOfLambda(ReadLambdaTarget(lambdaNode, sourceCode), table);
}

static uint32_t FindLambdaParamIndex(TSNode paramsNode, std::string_view paramName, std::string_view sourceCode)
{
    auto astParams = ExtractLambdaParameters(paramsNode, sourceCode);
    for (const auto& p : astParams)
    {
        if (p.name == paramName)
        {
            return p.index;
        }
    }
    return UINT32_MAX;
}

std::string InferLambdaParamType(TSNode nodeInLambda, std::string_view paramName, const SymbolTable& symbolTable,
                                 std::string_view sourceCode)
{
    if (ts_node_is_null(nodeInLambda) || paramName.empty())
    {
        return "";
    }

    TSNode lambdaNode = parser::FindAncestorWithSymbol(nodeInLambda, parser::GetGrammarSymbols().symLambdaExpression);
    if (ts_node_is_null(lambdaNode))
    {
        return "";
    }

    TSNode paramsNode = parser::GetChildByField(lambdaNode, parser::GetGrammarSymbols().fieldParameters);
    if (ts_node_is_null(paramsNode))
    {
        return "";
    }

    uint32_t paramIdx = FindLambdaParamIndex(paramsNode, paramName, sourceCode);
    if (paramIdx == UINT32_MAX)
    {
        return "";
    }

    auto targetFuncdef = FuncdefTargetOfLambda(lambdaNode, symbolTable, sourceCode);
    if (!targetFuncdef)
    {
        return "";
    }

    if (std::holds_alternative<FuncdefSignature>(targetFuncdef->signature))
    {
        const auto& params = targetFuncdef->GetFuncdef().parameters;
        return paramIdx < params.size() ? MemberOwnerType(params[paramIdx].typeName, symbolTable.GetDefaultArrayType())
                                        : "";
    }
    if (std::holds_alternative<FunctionSignature>(targetFuncdef->signature))
    {
        const auto& params = targetFuncdef->GetFunction().parameters;
        return paramIdx < params.size() ? MemberOwnerType(params[paramIdx].typeName, symbolTable.GetDefaultArrayType())
                                        : "";
    }
    return "";
}

static bool HasFunctionSymbolOnly(const std::vector<Symbol>& syms)
{
    bool hasFunc = false;
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Function)
        {
            hasFunc = true;
        }
        else if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
        {
            return false;
        }
    }
    return hasFunc;
}

static bool IsMemberMethodSymbol(TSNode node, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, ctx.nodeIndex);
    const TSNode objNode = syntax.Field(gs.fieldObject);
    const TSNode memNode = syntax.Field(gs.fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return false;
    }
    const std::string memName(parser::NodeText(memNode, ctx.sourceCode));
    const std::string objType = ResolveExpressionType(objNode, ctx);
    const std::string cleanObj = CleanBaseType(objType);
    if (cleanObj.empty() || memName.empty())
    {
        return false;
    }
    for (const auto& cls : GetInheritedTypeHierarchy(cleanObj, ctx.symbolTable))
    {
        if (const auto syms = ctx.symbolTable.FindMemberSymbolPtr(cls, memName))
        {
            if (HasFunctionSymbolOnly(*syms))
            {
                return true;
            }
        }
    }
    return false;
}

static bool IsLocalVariableOrFieldInScope(const Scope* scope, TSSymbol sym, const std::string& name,
                                          uint32_t refByte = UINT32_MAX)
{
    const auto& gs = parser::GetGrammarSymbols();
    const bool isUnqualified =
        (sym == gs.symIdentifier || (sym == gs.symScopedIdentifier && !utils::ContainsScopeSeparator(name)));
    if (isUnqualified && scope)
    {
        if (const auto def = ResolveInScopeAt(scope, name, refByte))
        {
            return def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter ||
                   def->kind == LocalDefinitionKind::Field;
        }
    }
    return false;
}

static bool HasGlobalFunctionSymbol(std::string_view name, const SymbolTable& symbolTable)
{
    if (const auto found = symbolTable.FindSymbolsPtr(name))
    {
        return HasFunctionSymbolOnly(*found);
    }
    const std::string_view lastSeg = LastScopeSegment(name);
    if (lastSeg != name)
    {
        if (const auto found = symbolTable.FindSymbolsPtr(lastSeg))
        {
            return HasFunctionSymbolOnly(*found);
        }
    }
    return false;
}

static bool IsIdentifierFunctionSymbol(TSNode node, const Scope* scope, const SymbolTable& symbolTable,
                                       std::string_view sourceCode)
{
    std::string name(parser::NodeText(node, sourceCode));
    if (name.empty())
    {
        return false;
    }
    if (IsLocalVariableOrFieldInScope(scope, ts_node_symbol(node), name, ts_node_start_byte(node)))
    {
        return false;
    }
    return HasGlobalFunctionSymbol(name, symbolTable);
}

bool IsFunctionReference(TSNode node, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    node = UnwrapIndexedParentheses(node, ctx.nodeIndex);
    if (ts_node_is_null(node))
    {
        return false;
    }
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression)
    {
        return false;
    }
    if (sym == gs.symMemberExpression)
    {
        return IsMemberMethodSymbol(node, ctx);
    }
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return IsIdentifierFunctionSymbol(node, ctx.scope, ctx.symbolTable, ctx.sourceCode);
    }
    return false;
}

bool IsFunctionReferenceAt(IndexedByteRange bytes, const ExpressionTypeContext& ctx)
{
    const TSNode node = RecoverNodeAt(ctx, bytes.startByte, bytes.endByte);
    if (ts_node_is_null(node))
    {
        return false;
    }
    return IsFunctionReference(node, ctx);
}

} // namespace angel_lsp::analysis
