#include "analysis/index/CallArgumentFacts.h"

#include "analysis/index/SyntaxOperands.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

TSNode ResolveArgumentListNode(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symArgumentList)
    {
        return node;
    }
    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression || sym == gs.symVariableDeclarator)
    {
        TSNode args = parser::GetChildByField(node, gs.fieldArguments);
        if (!ts_node_is_null(args))
        {
            return args;
        }
        TSTreeCursor cursor = ts_tree_cursor_new(node);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode ch = ts_tree_cursor_current_node(&cursor);
                if (ts_node_symbol(ch) == gs.symArgumentList)
                {
                    ts_tree_cursor_delete(&cursor);
                    return ch;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
            ts_tree_cursor_delete(&cursor);
        }
    }
    return TSNode{};
}

static void ProcessCallArgumentChild(TSNode child, TSFieldId fieldId, std::string_view sourceCode,
                                     CallArgumentInfo& current)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (fieldId == gs.fieldArgName)
    {
        current.nameNode = child;
        if (!sourceCode.empty())
        {
            current.name = std::string(parser::NodeText(child, sourceCode));
        }
        return;
    }
    current.exprNode = child;
}

std::vector<CallArgumentInfo> ExtractCallArguments(TSNode argumentList, std::string_view sourceCode)
{
    TSNode targetList = ResolveArgumentListNode(argumentList);
    if (ts_node_is_null(targetList))
    {
        return {};
    }

    std::vector<CallArgumentInfo> result;
    TSTreeCursor cursor = ts_tree_cursor_new(targetList);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return result;
    }

    CallArgumentInfo current;
    current.index = 0;
    bool inArgument = false;

    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(child);

        if (sym == gs.tokOpenParen || sym == gs.symComment || sym == gs.tokColon)
        {
            continue;
        }
        if (sym == gs.tokCloseParen)
        {
            break;
        }
        if (sym == gs.tokComma)
        {
            if (inArgument)
            {
                result.push_back(std::move(current));
                current = CallArgumentInfo{};
                current.index = static_cast<uint32_t>(result.size());
                inArgument = false;
            }
            continue;
        }

        if (inArgument && !ts_node_is_null(current.exprNode))
        {
            result.push_back(std::move(current));
            current = CallArgumentInfo{};
            current.index = static_cast<uint32_t>(result.size());
        }

        const TSFieldId fieldId = ts_tree_cursor_current_field_id(&cursor);
        ProcessCallArgumentChild(child, fieldId, sourceCode, current);
        inArgument = true;

    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);

    if (inArgument)
    {
        result.push_back(std::move(current));
    }

    return result;
}

size_t CountCallArguments(TSNode argumentList)
{
    return ExtractCallArguments(argumentList, "").size();
}

struct CallDelimiterState
{
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    int angleDepth = 0;
    bool inString = false;
    char stringChar = '\0';
    bool inLineComment = false;
    bool inBlockComment = false;

    [[nodiscard]] bool AtTopLevel() const noexcept
    {
        return parenDepth == 0 && bracketDepth == 0 && braceDepth == 0 && angleDepth == 0;
    }
};

static bool StepCommentState(std::string_view code, size_t& i, size_t cursorByte, CallDelimiterState& state)
{
    if (state.inLineComment)
    {
        if (code[i] == '\n')
        {
            state.inLineComment = false;
        }
        return true;
    }
    if (state.inBlockComment)
    {
        if (code[i] == '*' && i + 1 < cursorByte && code[i + 1] == '/')
        {
            state.inBlockComment = false;
            ++i;
        }
        return true;
    }
    if (code[i] == '/' && i + 1 < cursorByte)
    {
        if (code[i + 1] == '/')
        {
            state.inLineComment = true;
            ++i;
            return true;
        }
        if (code[i + 1] == '*')
        {
            state.inBlockComment = true;
            ++i;
            return true;
        }
    }
    return false;
}

static bool StepStringState(std::string_view code, size_t& i, size_t cursorByte, CallDelimiterState& state)
{
    if (state.inString)
    {
        if (code[i] == '\\' && i + 1 < cursorByte)
        {
            ++i;
        }
        else if (code[i] == state.stringChar)
        {
            state.inString = false;
        }
        return true;
    }
    if (code[i] == '"' || code[i] == '\'')
    {
        state.inString = true;
        state.stringChar = code[i];
        return true;
    }
    return false;
}

static void StepDelimiterDepth(char c, CallDelimiterState& state, uint32_t& activeParam)
{
    switch (c)
    {
    case '(':
        ++state.parenDepth;
        break;
    case ')':
        if (state.parenDepth > 0)
            --state.parenDepth;
        break;
    case '[':
        ++state.bracketDepth;
        break;
    case ']':
        if (state.bracketDepth > 0)
            --state.bracketDepth;
        break;
    case '{':
        ++state.braceDepth;
        break;
    case '}':
        if (state.braceDepth > 0)
            --state.braceDepth;
        break;
    case '<':
        ++state.angleDepth;
        break;
    case '>':
        if (state.angleDepth > 0)
            --state.angleDepth;
        break;
    case ',':
        if (state.AtTopLevel())
        {
            ++activeParam;
        }
        break;
    default:
        break;
    }
}

uint32_t CalculateActiveCallParameter(TSNode argListNode, size_t cursorByte, std::string_view sourceCode)
{
    TSNode targetList = ResolveArgumentListNode(argListNode);
    if (ts_node_is_null(targetList) || sourceCode.empty())
    {
        return 0;
    }

    uint32_t startByte = ts_node_start_byte(targetList);
    uint32_t endByte = ts_node_end_byte(targetList);
    if (cursorByte <= startByte)
    {
        return 0;
    }
    if (startByte < sourceCode.size() && sourceCode[startByte] == '(')
    {
        ++startByte;
    }

    const size_t bound = std::min(cursorByte, static_cast<size_t>(endByte));
    uint32_t activeParam = 0;
    CallDelimiterState state;

    for (size_t i = startByte; i < bound; ++i)
    {
        if (StepCommentState(sourceCode, i, bound, state) || StepStringState(sourceCode, i, bound, state))
        {
            continue;
        }
        StepDelimiterDepth(sourceCode[i], state, activeParam);
    }
    return activeParam;
}

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
