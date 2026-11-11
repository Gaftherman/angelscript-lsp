#include "analysis/index/CallArgumentFacts.h"

#include "analysis/index/SyntaxOperands.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

bool IsLambdaExpression(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    return ts_node_symbol(node) == parser::GetGrammarSymbols().symLambdaExpression;
}

static TSNode ResolveLambdaParameterListNode(TSNode node, const NodeIndex* index)
{
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symLambdaParameterList)
    {
        return node;
    }
    if (sym == gs.symLambdaExpression)
    {
        const SyntaxOperands operands(node, index);
        const TSNode params = operands.Field(gs.fieldParameters);
        return ts_node_is_null(params) ? operands.FirstNamed(gs.symLambdaParameterList) : params;
    }
    return TSNode{};
}

static void ApplyLambdaParamChild(TSNode child, TSFieldId fieldId, std::string_view sourceCode,
                                  LambdaParamASTInfo& current)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(child);
    if (fieldId == gs.fieldParamType)
    {
        current.typeNode = child;
        current.hasWrittenType = true;
        std::string_view text = parser::NodeText(child, sourceCode);
        current.isConst = IsConstTypeView(text);
        current.isHandle = IsHandleTypeView(text);
        current.typeName = CleanBaseType(text);
    }
    else if (sym == gs.tokAmp)
    {
        current.isReference = true;
    }
    else if (fieldId == gs.fieldDirection || sym == gs.tokIn || sym == gs.tokOut || sym == gs.tokInout)
    {
        current.isReference = true;
        current.modifier = (sym == gs.tokIn)    ? ParameterModifier::In
                           : (sym == gs.tokOut) ? ParameterModifier::Out
                                                : ParameterModifier::InOut;
    }
    else if (fieldId == gs.fieldName || sym == gs.symIdentifier)
    {
        current.nameNode = child;
        current.name = std::string(parser::NodeText(child, sourceCode));
    }
}

std::vector<LambdaParamASTInfo> ExtractLambdaParameters(TSNode lambdaParamList, std::string_view sourceCode,
                                                        const NodeIndex* index)
{
    TSNode targetList = ResolveLambdaParameterListNode(lambdaParamList, index);
    if (ts_node_is_null(targetList))
    {
        return {};
    }

    std::vector<LambdaParamASTInfo> result;
    LambdaParamASTInfo current;
    bool inParam = false;
    const auto& gs = parser::GetGrammarSymbols();
    SyntaxOperands(targetList, index)
        .ForEachChild(
            [&](TSNode child, TSFieldId field)
            {
                const TSSymbol sym = ts_node_symbol(child);
                if (sym == gs.tokCloseParen)
                {
                    return false;
                }
                if (sym == gs.tokOpenParen || sym == gs.symComment)
                {
                    return true;
                }
                if (sym == gs.tokComma)
                {
                    if (inParam)
                    {
                        result.push_back(std::move(current));
                        current = LambdaParamASTInfo{};
                        current.index = static_cast<uint32_t>(result.size());
                        inParam = false;
                    }
                    return true;
                }
                if (ts_node_is_null(current.startNode))
                {
                    current.startNode = child;
                }
                inParam = true;
                ApplyLambdaParamChild(child, field, sourceCode, current);
                return true;
            });

    if (inParam)
    {
        result.push_back(std::move(current));
    }
    return result;
}

std::vector<LambdaParameter> ReadLambdaParameters(TSNode listNode, std::string_view sourceCode)
{
    std::vector<LambdaParameter> parameters;
    auto astParams = ExtractLambdaParameters(listNode, sourceCode);
    parameters.reserve(astParams.size());
    for (const auto& p : astParams)
    {
        LambdaParameter lp;
        lp.typeName = p.typeName;
        lp.hasWrittenType = p.hasWrittenType;
        lp.isConst = p.isConst;
        lp.isHandle = p.isHandle;
        lp.isReference = p.isReference;
        lp.modifier = p.modifier;
        parameters.push_back(std::move(lp));
    }
    return parameters;
}

} // namespace angel_lsp::analysis
