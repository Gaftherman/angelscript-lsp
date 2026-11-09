#include "analysis/index/HoverFacts.h"
#include "analysis/index/HoverFactsInternal.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/Constants.h"

#include <array>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

std::string InferTypeFromAst(uint32_t nodeStartByte, uint32_t nodeEndByte, const ExpressionTypeContext& ctx)
{
    TSNode node = RecoverNodeAt(ctx, nodeStartByte, nodeEndByte);
    if (ts_node_is_null(node))
    {
        return "";
    }
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> declSyms = {gs.symParameter, gs.symVariableDeclaration};
    TSNode cur = parser::FindAncestorWithAnySymbol(node, declSyms);
    if (!ts_node_is_null(cur))
    {
        TSNode typeNode = parser::GetChildByField(cur, gs.fieldParamType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(cur, gs.fieldVarType);
        }
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(cur, gs.fieldType);
        }
        if (!ts_node_is_null(typeNode))
        {
            uint32_t tStart = ts_node_start_byte(typeNode);
            uint32_t tEnd = ts_node_end_byte(typeNode);
            if (tStart < ctx.sourceCode.size() && tEnd <= ctx.sourceCode.size() && tStart < tEnd)
            {
                return std::string(ctx.sourceCode.substr(tStart, tEnd - tStart));
            }
        }
    }
    return "";
}

std::optional<std::string> ResolveIdentifierDefaultValue(std::string_view name, uint32_t nodeStartByte,
                                                         uint32_t nodeEndByte, const ExpressionTypeContext& ctx)
{
    TSNode node = RecoverNodeAt(ctx, nodeStartByte, nodeEndByte);
    if (ctx.scope)
    {
        const LocalDefinition* def = analysis::ResolveInScopeAt(ctx.scope, name, nodeStartByte);
        if (def && !def->defaultValue.empty())
        {
            return std::string(parser::UnquoteStringView(def->defaultValue));
        }
    }
    auto candidates = analysis::FindSymbolsInScope(std::string(name), node, ctx.sourceCode, ctx.symbolTable);
    for (const auto& sym : candidates)
    {
        if (sym.type == SymbolType::Variable && !sym.GetVariable().defaultValue.empty())
        {
            return std::string(parser::UnquoteStringView(sym.GetVariable().defaultValue));
        }
    }
    if (const auto globals = ctx.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& sym : *globals)
        {
            if (sym.type == SymbolType::Variable && !sym.GetVariable().defaultValue.empty())
            {
                return std::string(parser::UnquoteStringView(sym.GetVariable().defaultValue));
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> EvaluateConstantStringExpr(uint32_t nodeStartByte, uint32_t nodeEndByte,
                                                      const ExpressionTypeContext& ctx)
{
    TSNode node = RecoverNodeAt(ctx, nodeStartByte, nodeEndByte);
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    std::string result;
    std::vector<TSNode> stack{node};
    while (!stack.empty())
    {
        TSNode current = stack.back();
        stack.pop_back();
        if (ts_node_is_null(current))
        {
            return std::nullopt;
        }

        const TSSymbol type = ts_node_symbol(current);
        if (type == gs.symStringLiteral)
        {
            std::string text(parser::NodeText(current, ctx.sourceCode));
            result += parser::UnquoteStringView(text);
        }
        else if (type == gs.symBinaryExpression)
        {
            TSNode opNode = parser::GetChildByField(current, gs.fieldOperator);
            if (ts_node_is_null(opNode) || ts_node_symbol(opNode) != gs.tokPlus)
            {
                return std::nullopt;
            }
            TSNode left = parser::GetChildByField(current, gs.fieldLeft);
            TSNode right = parser::GetChildByField(current, gs.fieldRight);
            if (ts_node_is_null(left) || ts_node_is_null(right))
            {
                return std::nullopt;
            }
            stack.push_back(right);
            stack.push_back(left);
        }
        else if (type == gs.symIdentifier || type == gs.symScopedIdentifier)
        {
            std::string name(parser::NodeText(current, ctx.sourceCode));
            auto val = ResolveIdentifierDefaultValue(name, ts_node_start_byte(current), ts_node_end_byte(current), ctx);
            if (!val.has_value())
            {
                return std::nullopt;
            }
            result += *val;
        }
        else
        {
            return std::nullopt;
        }
    }
    return result;
}

std::string FormatLambdaSignature(uint32_t lambdaStartByte, uint32_t lambdaEndByte, const ExpressionTypeContext& ctx)
{
    TSNode lambdaNode = RecoverNodeAt(ctx, lambdaStartByte, lambdaEndByte);
    if (ts_node_is_null(lambdaNode))
    {
        return "(anonymous function) function()";
    }

    std::ostringstream oss;
    oss << "(anonymous function) function";
    TSNode paramListNode = ts_node_child_by_field_name(lambdaNode, "parameters", 10);
    if (ts_node_is_null(paramListNode))
    {
        parser::ForEachNamedChildNodeUntil(lambdaNode,
                                           [&](TSNode child)
                                           {
                                               if (ts_node_symbol(child) ==
                                                   parser::GetGrammarSymbols().symParameterList)
                                               {
                                                   paramListNode = child;
                                                   return false;
                                               }
                                               return true;
                                           });
    }
    if (!ts_node_is_null(paramListNode))
    {
        uint32_t sb = ts_node_start_byte(paramListNode);
        uint32_t eb = ts_node_end_byte(paramListNode);
        if (sb < eb && eb <= ctx.sourceCode.size())
        {
            oss << ctx.sourceCode.substr(sb, eb - sb);
            return oss.str();
        }
    }
    oss << "()";
    return oss.str();
}

std::string DeduceLocalDefinitionHoverType(const LocalDefinition& def, IndexedByteRange hoverNode,
                                           const ExpressionTypeContext& ctx)
{
    if (!def.typeName.empty() && analysis::CleanBaseTypeView(def.typeName) != "auto")
    {
        return def.typeName;
    }
    std::string inferred = analysis::ResolveLocalDefinitionType(def, ctx);
    if (inferred.empty())
    {
        inferred = InferTypeFromAst(hoverNode.startByte, hoverNode.endByte, ctx);
    }
    if (inferred.empty() && def.kind == LocalDefinitionKind::Parameter)
    {
        TSNode node = RecoverNodeAt(ctx, hoverNode.startByte, hoverNode.endByte);
        inferred = analysis::InferLambdaParamType(node, def.name, ctx.symbolTable, ctx.sourceCode);
    }
    return !inferred.empty() ? inferred : def.typeName;
}

std::string DeduceAutoDeclaratorType(IndexedByteRange declaratorRange, IndexedByteRange hoverNode,
                                     const ExpressionTypeContext& ctx)
{
    TSNode declarator = RecoverNodeAt(ctx, declaratorRange.startByte, declaratorRange.endByte);
    if (ts_node_is_null(declarator))
    {
        return "";
    }

    TSNode nameNode = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldName);
    if (!ts_node_is_null(nameNode) && ctx.scope)
    {
        std::string varName(parser::NodeText(nameNode, ctx.sourceCode));
        if (const auto* def = analysis::ResolveInScopeAt(ctx.scope, varName, ts_node_start_byte(nameNode)))
        {
            std::string deduced = DeduceLocalDefinitionHoverType(*def, hoverNode, ctx);
            if (!deduced.empty() && deduced != "auto")
            {
                return deduced;
            }
        }
    }

    TSNode valNode = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(valNode))
    {
        valNode = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldArguments);
    }
    if (!ts_node_is_null(valNode))
    {
        std::string res = analysis::ResolveExpressionType(valNode, ctx);
        if (!res.empty() && res != "void" && res != "null" && res != constants::k_initializerListTypeSentinel &&
            !analysis::ExpressionTypeCache::IsPoisonType(res))
        {
            return res;
        }
    }
    return "";
}

} // namespace angel_lsp::analysis
