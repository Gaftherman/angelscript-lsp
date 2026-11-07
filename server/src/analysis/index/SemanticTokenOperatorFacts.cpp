#include "analysis/index/SemanticTokenFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/Utils.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>

namespace angel_lsp::analysis
{

/**
 * @brief Fast-path check using pre-resolved TSSymbols for template brackets.
 * @param[in] parentSym Grammar symbol of parent node.
 * @param[in] grandParentSym Grammar symbol of grandparent node.
 * @param[in] syms Cached grammar symbols.
 * @return True if symbols match template punctuation patterns.
 */
[[nodiscard]] static inline bool IsTemplatePunctuation(TSSymbol parentSym, TSSymbol grandParentSym,
                                                       const parser::GrammarSymbols& syms) noexcept
{
    if (parentSym == syms.symTemplateTypeList || parentSym == syms.symCastExpression ||
        parentSym == syms.symTemplateParameterList)
    {
        return true;
    }

    if (grandParentSym == syms.symTemplateTypeList || grandParentSym == syms.symCastExpression ||
        grandParentSym == syms.symTemplateParameterList)
    {
        if (parentSym != syms.symBinaryExpression && parentSym != syms.symAssignmentExpression &&
            parentSym != syms.symUnaryExpression && parentSym != syms.symPostfixExpression)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Trims leading and trailing ASCII whitespace from a string view.
 * @param[in] text String view to trim.
 * @return Trimmed string view.
 */
[[nodiscard]] static inline std::string_view TrimWhitespace(std::string_view text) noexcept
{
    return utils::TrimView(text);
}

bool IsPunctuationOrBracket(std::string_view text) noexcept
{
    text = TrimWhitespace(text);
    if (text.empty())
    {
        return false;
    }
    return text == "{" || text == "}" || text == "(" || text == ")" || text == "[" || text == "]" || text == ";" ||
           text == ",";
}

bool IsGenuineOperator(std::string_view text) noexcept
{
    text = TrimWhitespace(text);
    static const ankerl::unordered_dense::set<std::string_view> s_operators = {
        "+", "-",  "*", "/",  "%",  "++",  "--", "==", "!=", "<",   "<=", ">",   ">=",  "&&",   "||",
        "!", "^^", "=", "+=", "-=", "*=",  "/=", "%=", "&=", "|=",  "^=", "<<=", ">>=", ">>>=", "&",
        "|", "^",  "~", "<<", ">>", ">>>", "?",  ":",  "@=", "and", "or", "xor", "not", "is",   "!is"};
    return s_operators.contains(text);
}

/**
 * @brief Fast-path check using pre-resolved TSSymbols for reference modifier '&'.
 * @param[in] parentSym Grammar symbol of parent node.
 * @param[in] grandParentSym Grammar symbol of grandparent node.
 * @param[in] syms Cached grammar symbols.
 * @return True if symbols match reference modifier patterns.
 */
[[nodiscard]] static inline bool IsReferenceModifier(TSSymbol parentSym, TSSymbol grandParentSym,
                                                     const parser::GrammarSymbols& syms) noexcept
{
    if (parentSym == syms.symParameter || parentSym == syms.symType || parentSym == syms.symDatatype ||
        parentSym == syms.symTemplateTypeList || parentSym == syms.symCastExpression)
    {
        return true;
    }
    return grandParentSym == syms.symParameter || grandParentSym == syms.symType || grandParentSym == syms.symDatatype;
}

/**
 * @brief Checks if a node is an '&' used as a type or parameter reference modifier.
 * @param[in] node AST node for '&'.
 * @param[in] syms Cached grammar symbols.
 * @return True if node is enclosed in a type or parameter modifier construct.
 */
[[nodiscard]] static inline bool IsReferenceModifierNode(TSNode node, const parser::GrammarSymbols& syms) noexcept
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    TSNode grandParent = parser::GetParent(parent);
    TSSymbol parentSym = ts_node_symbol(parent);
    TSSymbol grandParentSym = !ts_node_is_null(grandParent) ? ts_node_symbol(grandParent) : 0;
    return IsReferenceModifier(parentSym, grandParentSym, syms);
}

std::optional<std::pair<uint32_t, int>> ClassifyOperatorOrPunctuation(TSNode node, std::string_view sourceCode,
                                                                      const parser::GrammarSymbols& syms) noexcept
{
    const uint32_t sb = ts_node_start_byte(node);
    const uint32_t eb = ts_node_end_byte(node);
    if (sb >= eb || eb > sourceCode.size())
    {
        return std::nullopt;
    }
    std::string_view text(sourceCode.data() + sb, eb - sb);
    if (text == "@")
    {
        return std::make_pair(Type_Modifier, 8);
    }
    if (text == "&" && IsReferenceModifierNode(node, syms))
    {
        return std::make_pair(Type_Modifier, 8);
    }
    if (IsPunctuationOrBracket(text) || !IsGenuineOperator(text))
    {
        return std::nullopt;
    }
    return std::make_pair(Type_Operator, 4);
}

bool IsTemplatePunctuationNode(TSNode node, const parser::GrammarSymbols& syms) noexcept
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    TSNode grandParent = parser::GetParent(parent);
    TSSymbol parentSym = ts_node_symbol(parent);
    TSSymbol grandParentSym = !ts_node_is_null(grandParent) ? ts_node_symbol(grandParent) : 0;
    return IsTemplatePunctuation(parentSym, grandParentSym, syms);
}

} // namespace angel_lsp::analysis
