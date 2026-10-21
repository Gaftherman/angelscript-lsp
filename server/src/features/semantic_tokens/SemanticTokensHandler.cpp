#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/QueryRegistry.h"
#include "parser/queries/BuiltQueries.h"
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::features
{
namespace
{
enum TokenTypeIndex : uint32_t
{
    Type_Namespace = 0,
    Type_Type = 1,
    Type_Class = 2,
    Type_Enum = 3,
    Type_Interface = 4,
    Type_Struct = 5,
    Type_TypeParameter = 6,
    Type_Parameter = 7,
    Type_Variable = 8,
    Type_Property = 9,
    Type_EnumMember = 10,
    Type_Event = 11,
    Type_Function = 12,
    Type_Method = 13,
    Type_Macro = 14,
    Type_Keyword = 15,
    Type_Modifier = 16,
    Type_Comment = 17,
    Type_String = 18,
    Type_Number = 19,
    Type_Regexp = 20,
    Type_Operator = 21,
    Type_Decorator = 22,
    Type_TemplatePunctuation = 23
};

enum TokenModifierBit : uint32_t
{
    Mod_Declaration = 1 << 0,
    Mod_Definition = 1 << 1,
    Mod_Readonly = 1 << 2,
    Mod_Static = 1 << 3,
    Mod_Deprecated = 1 << 4,
    Mod_Abstract = 1 << 5,
    Mod_Async = 1 << 6,
    Mod_Modification = 1 << 7,
    Mod_Documentation = 1 << 8,
    Mod_DefaultLibrary = 1 << 9
};

/**
 * @brief Metadata associated with a classified semantic token.
 */
struct TokenMeta
{
    uint32_t tokenType = 0;
    uint32_t tokenMod = 0;
    int priority = 0;
};

/**
 * @brief Packs a line and character into a 64-bit coordinate key.
 * @param[in] line Zero-based source line.
 * @param[in] character Zero-based column offset.
 * @return 64-bit position key.
 */
constexpr uint64_t PositionKey(uint32_t line, uint32_t character)
{
    return (static_cast<uint64_t>(line) << 32) | character;
}

/**
 * @brief Resolves every non-member identifier reference in the scope tree to its declaration kind.
 * @param[in] scope Scope root or child node.
 * @param[in,out] out Map from position key to resolved LocalDefinitionKind.
 */
void CollectReferenceKinds(const analysis::Scope* scope,
                           ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& out)
{
    for (const auto& ref : scope->references)
    {
        if (ref.isMemberAccess)
        {
            continue;
        }

        if (const analysis::LocalDefinition* def = analysis::ResolveInScope(scope, ref.name))
        {
            out[PositionKey(ref.startLine, ref.startCharacter)] = def->kind;
        }
    }

    for (const auto& child : scope->children)
    {
        CollectReferenceKinds(child.get(), out);
    }
}

/**
 * @brief Represents an intermediate, non-delta-encoded semantic token item.
 */
struct RawToken
{
    uint32_t line = 0;
    uint32_t startChar = 0;
    uint32_t length = 0;
    uint32_t tokenType = 0;
    uint32_t tokenModifiers = 0;
    int priority = 0;
};

/**
 * @brief Splits source code into views of individual lines without trailing carriage returns.
 * @param[in] str Full source buffer.
 * @return Vector of line views.
 */
std::vector<std::string_view> SplitLinesView(std::string_view str)
{
    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] == '\n')
        {
            size_t len = i - start;
            if (len > 0 && str[i - 1] == '\r')
            {
                len--;
            }
            lines.emplace_back(str.data() + start, len);
            start = i + 1;
        }
    }
    if (start < str.size())
    {
        size_t len = str.size() - start;
        if (len > 0 && str.back() == '\r')
        {
            len--;
        }
        lines.emplace_back(str.data() + start, len);
    }
    return lines;
}

using parser::GetGrammarSymbols;
using parser::GrammarSymbols;

/**
 * @brief Fast-path check using pre-resolved TSSymbols for template brackets.
 * @param[in] parentSym Grammar symbol of parent node.
 * @param[in] grandParentSym Grammar symbol of grandparent node.
 * @param[in] syms Cached grammar symbols.
 * @return True if symbols match template punctuation patterns.
 */
[[nodiscard]] inline bool IsTemplatePunctuation(TSSymbol parentSym, TSSymbol grandParentSym,
                                                const GrammarSymbols& syms) noexcept
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
[[nodiscard]] inline std::string_view TrimWhitespace(std::string_view text) noexcept
{
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
    {
        text.remove_suffix(1);
    }
    return text;
}

/**
 * @brief Checks whether the given text is punctuation or bracket that must never be an operator.
 * @param[in] text Token text.
 * @return True if token is punctuation or bracket.
 */
[[nodiscard]] inline bool IsPunctuationOrBracket(std::string_view text) noexcept
{
    text = TrimWhitespace(text);
    if (text.empty())
    {
        return false;
    }
    return text == "{" || text == "}" || text == "(" || text == ")" || text == "[" || text == "]" || text == ";" ||
           text == ",";
}

/**
 * @brief Checks whether the given text is a genuine AngelScript operator token.
 * @param[in] text Token text.
 * @return True if token is genuine operator.
 */
[[nodiscard]] inline bool IsGenuineOperator(std::string_view text) noexcept
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
[[nodiscard]] inline bool IsReferenceModifier(TSSymbol parentSym, TSSymbol grandParentSym,
                                              const GrammarSymbols& syms) noexcept
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
[[nodiscard]] inline bool IsReferenceModifierNode(TSNode node, const GrammarSymbols& syms) noexcept
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

/**
 * @brief Classifies an operator or punctuation capture node into token type and priority.
 * @param[in] node AST capture node.
 * @param[in] sourceCode Document source text.
 * @param[in] syms Cached grammar symbols.
 * @return Optional pair of (tokenType, priority), or nullopt if ignored.
 */
[[nodiscard]] inline std::optional<std::pair<uint32_t, int>>
ClassifyOperatorOrPunctuation(TSNode node, std::string_view sourceCode, const GrammarSymbols& syms) noexcept
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

/**
 * @brief Classification rule for a single tree-sitter query capture pattern.
 */
struct CaptureRule
{
    uint32_t tokenType = Type_Variable;
    uint32_t tokenMod = 0;
    int priority = 1;
    bool isOperatorOrPunctuation = false;
    bool valid = false;
};

/**
 * @brief Matches special syntactic capture patterns like comments, literals, or punctuation.
 * @param[in] name Capture name.
 * @param[out] rule Output classification rule.
 * @return True if pattern matched.
 */
bool MatchSpecialCaptureRule(std::string_view name, CaptureRule& rule)
{
    if (name == "comment" || name == "string")
    {
        rule.tokenType = (name == "comment") ? Type_Comment : Type_String;
        rule.priority = 10;
        rule.valid = true;
        return true;
    }
    if (name == "number")
    {
        rule.tokenType = Type_Number;
        rule.priority = 9;
        rule.valid = true;
        return true;
    }
    if (name == "keyword.directive")
    {
        rule.tokenType = Type_Macro;
        rule.priority = 8;
        rule.valid = true;
        return true;
    }
    if (name == "template.list")
    {
        rule.tokenType = Type_TemplatePunctuation;
        rule.priority = 4;
        rule.valid = true;
        return true;
    }
    if (name == "operator" || name == "punctuation.special")
    {
        rule.isOperatorOrPunctuation = true;
        rule.valid = true;
        return true;
    }
    return false;
}

/**
 * @brief Matches keywords, modifiers, and constants.
 * @param[in] name Capture name.
 * @param[out] rule Output classification rule.
 * @return True if pattern matched.
 */
bool MatchKeywordOrConstantRule(std::string_view name, CaptureRule& rule)
{
    if (name == "keyword" || name == "keyword.control" || name == "keyword.operator" || name == "boolean")
    {
        rule.tokenType = Type_Keyword;
        rule.priority = 8;
        rule.valid = true;
        return true;
    }
    if (name == "keyword.modifier")
    {
        rule.tokenType = Type_Modifier;
        rule.priority = 8;
        rule.valid = true;
        return true;
    }
    if (name == "constant.builtin")
    {
        rule.tokenType = Type_Keyword;
        rule.tokenMod = Mod_Readonly;
        rule.priority = 8;
        rule.valid = true;
        return true;
    }
    if (name == "constant")
    {
        rule.tokenType = Type_EnumMember;
        rule.priority = 5;
        rule.valid = true;
        return true;
    }
    return false;
}

/**
 * @brief Matches function, method, and property capture patterns.
 * @param[in] name Capture name.
 * @param[out] rule Output classification rule.
 * @return True if pattern matched.
 */
bool MatchCallableOrPropertyRule(std::string_view name, CaptureRule& rule)
{
    if (name == "function" || name == "function.method")
    {
        rule.tokenType = (name == "function.method") ? Type_Method : Type_Function;
        rule.tokenMod = Mod_Declaration;
        rule.priority = (name == "function.method") ? 7 : 6;
        rule.valid = true;
        return true;
    }
    if (name == "function.call" || name == "function.method.call" || name == "property")
    {
        rule.tokenType = (name == "property") ? Type_Property : (name == "function.call" ? Type_Function : Type_Method);
        rule.priority = 5;
        rule.valid = true;
        return true;
    }
    if (name == "property.access")
    {
        rule.tokenType = Type_Property;
        rule.priority = 3;
        rule.valid = true;
        return true;
    }
    return false;
}

/**
 * @brief Matches types, functions, variables, and properties.
 * @param[in] name Capture name.
 * @param[out] rule Output classification rule.
 * @return True if pattern matched.
 */
bool MatchEntityCaptureRule(std::string_view name, CaptureRule& rule)
{
    if (name == "type.builtin")
    {
        rule.tokenType = Type_Type;
        rule.tokenMod = Mod_DefaultLibrary;
        rule.priority = 7;
        rule.valid = true;
        return true;
    }
    if (name == "type" || name == "module")
    {
        rule.tokenType = (name == "module") ? Type_Namespace : Type_Type;
        rule.priority = 6;
        rule.valid = true;
        return true;
    }
    if (MatchCallableOrPropertyRule(name, rule))
    {
        return true;
    }
    if (name == "variable" || name == "variable.parameter" || name == "variable.parameter.declaration")
    {
        rule.tokenType = (name == "variable") ? Type_Variable : Type_Parameter;
        rule.tokenMod = (name == "variable.parameter.declaration") ? Mod_Declaration : 0;
        rule.priority = (name == "variable") ? 3 : 4;
        rule.valid = true;
        return true;
    }
    return false;
}

/**
 * @brief Converts a tree-sitter capture name into a semantic CaptureRule.
 * @param[in] name Capture identifier string.
 * @return Configured CaptureRule.
 */
CaptureRule MakeCaptureRule(std::string_view name)
{
    CaptureRule rule;
    if (MatchSpecialCaptureRule(name, rule) || MatchKeywordOrConstantRule(name, rule) ||
        MatchEntityCaptureRule(name, rule))
    {
        return rule;
    }
    rule.valid = false;
    return rule;
}

/**
 * @brief Holds precompiled highlights query and per-capture classification rules.
 */
struct HighlightsQueryData
{
    TSQuery* query = nullptr;
    std::vector<CaptureRule> rules;
};

/**
 * @brief Returns precompiled highlights TSQuery and cached capture rules.
 * @return Reference to static HighlightsQueryData.
 */
const HighlightsQueryData& GetHighlightsQueryData()
{
    static const HighlightsQueryData s_data = []() -> HighlightsQueryData
    {
        const TSLanguage* lang = tree_sitter_angelscript();
        uint32_t errorOffset = 0;
        TSQueryError errorType = TSQueryErrorNone;
        TSQuery* query =
            ts_query_new(lang, parser::queries::HIGHLIGHTS_QUERY,
                         static_cast<uint32_t>(strlen(parser::queries::HIGHLIGHTS_QUERY)), &errorOffset, &errorType);
        if (!query)
        {
            return HighlightsQueryData{};
        }

        const uint32_t count = ts_query_capture_count(query);
        std::vector<CaptureRule> rules;
        rules.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t nameLen = 0;
            const char* namePtr = ts_query_capture_name_for_id(query, i, &nameLen);
            rules.push_back(MakeCaptureRule(std::string_view(namePtr, nameLen)));
        }
        return HighlightsQueryData{query, std::move(rules)};
    }();
    return s_data;
}

/**
 * @brief Records a byte-range span of an enclosing class.
 */
struct ClassSpan
{
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    std::string name;
    int parent = -1;
};

/**
 * @brief Index of enclosing class spans for fast byte-offset hierarchy lookup.
 */
class ClassSpanIndex
{
  public:
    /**
     * @brief Builds class span index from NodeIndex and source code.
     * @param[in] nodeIndex Pre-built AST node index, or nullptr.
     * @param[in] sourceCode Source text buffer.
     * @param[in] syms Grammar symbol identifiers.
     * @return Initialized ClassSpanIndex.
     */
    static ClassSpanIndex Build(const analysis::NodeIndex* nodeIndex, std::string_view sourceCode,
                                const GrammarSymbols& syms)
    {
        ClassSpanIndex index;
        if (!nodeIndex)
        {
            return index;
        }
        for (TSNode classDecl : nodeIndex->Nodes(syms.symClassDeclaration))
        {
            TSNode classNameNode = parser::GetChildByField(classDecl, parser::fields::Name);
            if (!ts_node_is_null(classNameNode))
            {
                uint32_t cStart = ts_node_start_byte(classNameNode);
                uint32_t cEnd = ts_node_end_byte(classNameNode);
                if (cStart < cEnd && cEnd <= sourceCode.size())
                {
                    std::string_view bareName(sourceCode.data() + cStart, cEnd - cStart);
                    std::string qName;
                    const auto containers = analysis::GetEnclosingContainers(classDecl, sourceCode);
                    if (!containers.empty() && !containers.front().qualifiedName.empty())
                    {
                        qName = containers.front().qualifiedName + "::" + std::string(bareName);
                    }
                    else
                    {
                        qName = std::string(bareName);
                    }
                    index.m_spans.push_back(
                        ClassSpan{ts_node_start_byte(classDecl), ts_node_end_byte(classDecl), std::move(qName), -1});
                }
            }
        }

        std::sort(index.m_spans.begin(), index.m_spans.end(),
                  [](const ClassSpan& a, const ClassSpan& b)
                  {
                      if (a.startByte != b.startByte)
                      {
                          return a.startByte < b.startByte;
                      }
                      return a.endByte > b.endByte;
                  });

        std::vector<int> spanStack;
        for (size_t i = 0; i < index.m_spans.size(); ++i)
        {
            while (!spanStack.empty() && index.m_spans[spanStack.back()].endByte <= index.m_spans[i].startByte)
            {
                spanStack.pop_back();
            }
            if (!spanStack.empty() && index.m_spans[spanStack.back()].endByte >= index.m_spans[i].endByte)
            {
                index.m_spans[i].parent = spanStack.back();
            }
            spanStack.push_back(static_cast<int>(i));
        }
        return index;
    }

    /**
     * @brief Finds enclosing class name for a byte offset.
     * @param[in] byteOffset Target byte position in source.
     * @return Enclosing class name view, or empty if outside classes.
     */
    [[nodiscard]] std::string_view FindEnclosingClass(uint32_t byteOffset) const
    {
        if (m_spans.empty())
        {
            return {};
        }

        auto it = std::upper_bound(m_spans.begin(), m_spans.end(), byteOffset,
                                   [](uint32_t offset, const ClassSpan& span) { return offset < span.startByte; });

        if (it == m_spans.begin())
        {
            return {};
        }

        const int cand = static_cast<int>(std::distance(m_spans.begin(), it)) - 1;
        for (int cur = cand; cur != -1; cur = m_spans[cur].parent)
        {
            if (byteOffset >= m_spans[cur].startByte && byteOffset < m_spans[cur].endByte)
            {
                return m_spans[cur].name;
            }
        }
        return {};
    }

  private:
    std::vector<ClassSpan> m_spans;
};

/**
 * @brief Checks if any symbol in a list is an enum constant.
 * @param[in] symbols Symbol vector to inspect.
 * @return True if at least one symbol is an enum constant.
 */
bool ContainsEnumConstant(const std::shared_ptr<const std::vector<analysis::Symbol>>& symbols)
{
    if (!symbols || symbols->empty())
    {
        return false;
    }
    return std::any_of(symbols->begin(), symbols->end(), [](const analysis::Symbol& sym)
                       { return sym.type == analysis::SymbolType::Variable && sym.GetVariable().isEnumConstant; });
}

/**
 * @brief Checks if all symbols in a non-empty list are enum types.
 * @param[in] symbols Symbol vector to inspect.
 * @return True if all symbols have SymbolType::Enum.
 */
bool AreAllEnumTypes(const std::shared_ptr<const std::vector<analysis::Symbol>>& symbols)
{
    return symbols && !symbols->empty() &&
           std::all_of(symbols->begin(), symbols->end(),
                       [](const analysis::Symbol& sym) { return sym.type == analysis::SymbolType::Enum; });
}

/**
 * @brief Applies RuleIndex-based enum qualifier and member upgrades for a scoped_identifier.
 * @param[in] scopedNode AST node of type scoped_identifier.
 * @param[in] sourceCode Source text buffer.
 * @param[in] ruleIndex Pre-indexed workspace symbol metadata.
 * @param[in,out] upgrades Map from start byte to upgraded token type.
 */
void ApplyRuleIndexEnumUpgrades(TSNode scopedNode, std::string_view sourceCode,
                                const analysis::rules::RuleIndex& ruleIndex,
                                ankerl::unordered_dense::map<uint32_t, uint32_t>& upgrades)
{
    const uint32_t namedCount = ts_node_named_child_count(scopedNode);
    TSNode leftNode = ts_node_named_child(scopedNode, 0);
    TSNode rightNode = ts_node_named_child(scopedNode, namedCount - 1);
    TSNode prevNode = ts_node_named_child(scopedNode, namedCount - 2);
    const std::string_view leftText = analysis::GetNodeTextView(leftNode, sourceCode);
    const std::string_view rightText = analysis::GetNodeTextView(rightNode, sourceCode);
    const std::string_view prevText = analysis::GetNodeTextView(prevNode, sourceCode);
    auto enumIt = ruleIndex.enumSymbolsByMemberName.find(std::string(rightText));
    if (enumIt == ruleIndex.enumSymbolsByMemberName.end())
    {
        return;
    }
    const uint32_t rightStart = ts_node_start_byte(rightNode);
    for (const auto& enumSym : enumIt->second)
    {
        if (prevText == enumSym.name)
        {
            upgrades[ts_node_start_byte(prevNode)] = Type_Enum;
            upgrades[rightStart] = Type_EnumMember;
            return;
        }
        if (leftText == enumSym.containerName || prevText == enumSym.containerName ||
            prevText == analysis::LastScopeSegment(enumSym.containerName))
        {
            upgrades[rightStart] = Type_EnumMember;
        }
    }
}

/**
 * @brief Upgrades a single scoped_identifier node's enum and enum-member segments.
 * @param[in] scopedNode AST node of type scoped_identifier.
 * @param[in] sourceCode Source text buffer.
 * @param[in] symbolTable Global symbol table.
 * @param[in,out] upgrades Map from start byte to upgraded token type.
 */
void UpgradeScopedIdentifierEnumTokens(TSNode scopedNode, std::string_view sourceCode,
                                       const analysis::SymbolTable& symbolTable,
                                       ankerl::unordered_dense::map<uint32_t, uint32_t>& upgrades)
{
    const uint32_t namedCount = ts_node_named_child_count(scopedNode);
    if (namedCount < 2)
    {
        return;
    }
    TSNode leftNode = ts_node_named_child(scopedNode, 0);
    TSNode rightNode = ts_node_named_child(scopedNode, namedCount - 1);
    const std::string_view leftText = analysis::GetNodeTextView(leftNode, sourceCode);
    const std::string_view rightText = analysis::GetNodeTextView(rightNode, sourceCode);
    if (leftText.empty() || rightText.empty())
    {
        return;
    }

    const uint32_t leftStart = ts_node_start_byte(leftNode);
    const uint32_t rightStart = ts_node_start_byte(rightNode);
    if (AreAllEnumTypes(symbolTable.FindSymbolsPtr(leftText)))
    {
        upgrades[leftStart] = Type_Enum;
        upgrades[rightStart] = Type_EnumMember;
        return;
    }
    if (ContainsEnumConstant(symbolTable.FindSymbolsPtr(analysis::GetNodeTextView(scopedNode, sourceCode))))
    {
        upgrades[rightStart] = Type_EnumMember;
    }
    if (const auto ruleIndex = symbolTable.GetRuleIndex())
    {
        ApplyRuleIndexEnumUpgrades(scopedNode, sourceCode, *ruleIndex, upgrades);
    }
}

/**
 * @brief Upgrades scoped enum type qualifiers and member tokens in type AST nodes.
 * @param[in] typeNode Type AST node.
 * @param[in] sourceCode Source text buffer.
 * @param[in] symbolTable Global symbol table.
 * @param[in,out] upgrades Map from start byte to upgraded token type.
 */
void UpgradeScopedTypeEnumTokens(TSNode typeNode, std::string_view sourceCode, const analysis::SymbolTable& symbolTable,
                                 ankerl::unordered_dense::map<uint32_t, uint32_t>& upgrades)
{
    if (ts_node_named_child_count(typeNode) < 2)
    {
        return;
    }
    const auto& syms = GetGrammarSymbols();
    TSNode firstChild = ts_node_named_child(typeNode, 0);
    TSNode secondChild = ts_node_named_child(typeNode, 1);
    if (ts_node_symbol(firstChild) != syms.symScope || ts_node_symbol(secondChild) != syms.symDatatype)
    {
        return;
    }

    std::string_view scopeText = analysis::GetNodeTextView(firstChild, sourceCode);
    while (scopeText.ends_with(':'))
    {
        scopeText.remove_suffix(1);
    }
    if (scopeText.starts_with("::"))
    {
        scopeText.remove_prefix(2);
    }
    const std::string_view memberText = analysis::GetNodeTextView(secondChild, sourceCode);
    if (scopeText.empty() || memberText.empty())
    {
        return;
    }

    TSNode scopeId = ts_node_named_child(firstChild, 0);
    if (!ts_node_is_null(scopeId) && ts_node_named_child_count(scopeId) > 0)
    {
        scopeId = ts_node_named_child(scopeId, ts_node_named_child_count(scopeId) - 1);
    }
    TSNode memberId = ts_node_named_child(secondChild, 0);

    const uint32_t scopeStart = ts_node_is_null(scopeId) ? ts_node_start_byte(firstChild) : ts_node_start_byte(scopeId);
    const uint32_t memberStart =
        ts_node_is_null(memberId) ? ts_node_start_byte(secondChild) : ts_node_start_byte(memberId);

    if (AreAllEnumTypes(symbolTable.FindSymbolsPtr(scopeText)))
    {
        upgrades[scopeStart] = Type_Enum;
        upgrades[memberStart] = Type_EnumMember;
        return;
    }
    const std::string qualifiedName = std::string(scopeText) + "::" + std::string(memberText);
    if (ContainsEnumConstant(symbolTable.FindSymbolsPtr(qualifiedName)))
    {
        upgrades[memberStart] = Type_EnumMember;
    }
}

/**
 * @brief Pre-calculates scoped enum qualifier and member token upgrades.
 * @param[in] nodeIndex Pre-indexed AST, or nullptr.
 * @param[in] sourceCode Source text buffer.
 * @param[in] symbolTable Global symbol table.
 * @param[in] syms Grammar symbols.
 * @return Map from start byte to upgraded token type.
 */
ankerl::unordered_dense::map<uint32_t, uint32_t>
PrecalculateScopedEnumUpgrades(const analysis::NodeIndex* nodeIndex, std::string_view sourceCode,
                               const analysis::SymbolTable& symbolTable, const GrammarSymbols& syms)
{
    ankerl::unordered_dense::map<uint32_t, uint32_t> upgrades;
    if (!nodeIndex)
    {
        return upgrades;
    }

    for (TSNode scopedNode : nodeIndex->Nodes(syms.symScopedIdentifier))
    {
        UpgradeScopedIdentifierEnumTokens(scopedNode, sourceCode, symbolTable, upgrades);
    }
    for (TSNode scopeNode : nodeIndex->Nodes(syms.symScope))
    {
        TSNode parentNode = parser::GetParent(scopeNode);
        if (!ts_node_is_null(parentNode) && ts_node_symbol(parentNode) == syms.symType)
        {
            UpgradeScopedTypeEnumTokens(parentNode, sourceCode, symbolTable, upgrades);
        }
    }
    return upgrades;
}

/**
 * @brief Context bundling precomputed indices and request references for token refinement.
 */
struct TokenRefinementContext
{
    const SemanticTokensRequest& request;
    const ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& referenceKinds;
    const ankerl::unordered_dense::map<uint32_t, uint32_t>& scopedEnumUpgrades;
    const ClassSpanIndex& classSpans;
    std::shared_ptr<const analysis::rules::RuleIndex> ruleIndex;
    mutable ankerl::unordered_dense::map<std::string_view, std::vector<std::string>> classHierarchyCache;
};

/**
 * @brief Checks whether an identifier is an unqualified member of the enclosing class.
 * @param[in] tokenText Identifier text.
 * @param[in] startByte Token start byte.
 * @param[in] tokenType Current token type.
 * @param[in] ctx Refinement context.
 * @return Upgraded token type (Type_Property, Type_Method, or unchanged).
 */
uint32_t RefineEnclosingClassMember(std::string_view tokenText, uint32_t startByte, uint32_t tokenType,
                                    const TokenRefinementContext& ctx)
{
    std::string_view className = ctx.classSpans.FindEnclosingClass(startByte);
    if (className.empty() || !ctx.ruleIndex)
    {
        return tokenType;
    }

    auto hit = ctx.classHierarchyCache.find(className);
    if (hit == ctx.classHierarchyCache.end())
    {
        auto hierarchy = analysis::GetInheritedTypeHierarchy(std::string(className), ctx.request.symbolTable);
        hit = ctx.classHierarchyCache.emplace(className, std::move(hierarchy)).first;
    }

    for (const auto& owner : hit->second)
    {
        const auto& ownerMembers = ctx.ruleIndex->Members(owner);
        if (tokenType == Type_Function && ownerMembers.methodNames.contains(tokenText))
        {
            return Type_Method;
        }
        if (tokenType == Type_Variable && ownerMembers.allMemberNames.contains(tokenText))
        {
            return Type_Property;
        }
    }
    return tokenType;
}

/**
 * @brief Matches agreed type with coarse Type_Type token.
 * @param[in] agreedType Consensus symbol type.
 * @param[in] isEnumConstant True if symbol is an enum constant.
 * @return Refined token type.
 */
[[nodiscard]] inline uint32_t MatchAgreedTypeToken(analysis::SymbolType agreedType, bool isEnumConstant) noexcept
{
    if (agreedType == analysis::SymbolType::Class)
        return Type_Class;
    if (agreedType == analysis::SymbolType::Interface)
        return Type_Interface;
    if (agreedType == analysis::SymbolType::Enum)
        return Type_Enum;
    if (agreedType == analysis::SymbolType::Variable && isEnumConstant)
        return Type_EnumMember;
    return Type_Type;
}

/**
 * @brief Matches agreed type with coarse Type_Namespace token.
 * @param[in] agreedType Consensus symbol type.
 * @return Refined token type.
 */
[[nodiscard]] inline uint32_t MatchAgreedNamespaceToken(analysis::SymbolType agreedType) noexcept
{
    if (agreedType == analysis::SymbolType::Enum)
        return Type_Enum;
    if (agreedType == analysis::SymbolType::Class)
        return Type_Class;
    return Type_Namespace;
}

/**
 * @brief Matches agreed variable symbol attributes to refined token type.
 * @param[in] isEnumConstant True if symbol is an enum constant.
 * @param[in] hasContainer True if symbol belongs to a container.
 * @return Refined token type.
 */
[[nodiscard]] inline uint32_t MatchAgreedVariableToken(bool isEnumConstant, bool hasContainer) noexcept
{
    if (isEnumConstant)
    {
        return Type_EnumMember;
    }
    if (hasContainer)
    {
        return Type_Property;
    }
    return Type_Variable;
}

/**
 * @brief Matches agreed SymbolType with current coarse token type.
 * @param[in] agreedType Consensus symbol type.
 * @param[in] tokenType Current coarse token type.
 * @param[in] hasContainer True if symbol belongs to a container.
 * @param[in] isEnumConstant True if symbol is an enum constant.
 * @return Refined token type.
 */
uint32_t MatchAgreedSymbolType(analysis::SymbolType agreedType, uint32_t tokenType, bool hasContainer,
                               bool isEnumConstant)
{
    if (tokenType == Type_Type)
    {
        return MatchAgreedTypeToken(agreedType, isEnumConstant);
    }
    if (tokenType == Type_Namespace)
    {
        return MatchAgreedNamespaceToken(agreedType);
    }
    if (agreedType == analysis::SymbolType::Function && tokenType == Type_Function && hasContainer)
    {
        return Type_Method;
    }
    if (agreedType == analysis::SymbolType::Variable && tokenType == Type_Variable)
    {
        return MatchAgreedVariableToken(isEnumConstant, hasContainer);
    }
    return tokenType;
}

/**
 * @brief Fallback refinement using RuleIndex for namespace-scoped enums and types.
 * @param[in] tokenText Identifier text.
 * @param[in] tokenType Current token type.
 * @param[in] ctx Refinement context.
 * @return Refined token type.
 */
uint32_t RefineRuleIndexFallback(std::string_view tokenText, uint32_t tokenType, const TokenRefinementContext& ctx)
{
    if (!ctx.ruleIndex)
    {
        return tokenType;
    }
    if ((tokenType == Type_Variable || tokenType == Type_Type) && ctx.ruleIndex->enumMemberNames.contains(tokenText))
    {
        return Type_EnumMember;
    }
    if (tokenType == Type_Variable && (ctx.ruleIndex->globalAccessorPropertyNames.contains(tokenText) ||
                                       ctx.ruleIndex->keywordGlobalAccessorPropertyNames.contains(tokenText)))
    {
        return Type_Property;
    }
    if (tokenType == Type_Type)
    {
        auto it = ctx.ruleIndex->qualifiedTypesByShortName.find(tokenText);
        if (it != ctx.ruleIndex->qualifiedTypesByShortName.end() && !it->second.empty())
        {
            const auto syms = ctx.request.symbolTable.FindSymbolsPtr(it->second.front());
            if (syms && !syms->empty())
            {
                return MatchAgreedSymbolType(syms->front().type, tokenType, true, false);
            }
        }
    }
    return tokenType;
}

/**
 * @brief Matches an identifier with symbolTable declarations to upgrade coarse tokens.
 * @param[in] tokenText Identifier text.
 * @param[in] tokenType Current token type.
 * @param[in] ctx Refinement context.
 * @return Refined token type.
 */
uint32_t RefineSymbolTableMatch(std::string_view tokenText, uint32_t tokenType, const TokenRefinementContext& ctx)
{
    const auto symbols = ctx.request.symbolTable.FindSymbolsPtr(tokenText);
    if (!symbols || symbols->empty())
    {
        return RefineRuleIndexFallback(tokenText, tokenType, ctx);
    }
    const analysis::SymbolType agreedType = symbols->front().type;
    bool allSymbolsMatch = std::all_of(symbols->begin(), symbols->end(),
                                       [agreedType](const analysis::Symbol& s) { return s.type == agreedType; });
    if (!allSymbolsMatch)
    {
        return tokenType;
    }
    bool isEnumConstant = false;
    if (agreedType == analysis::SymbolType::Variable &&
        std::holds_alternative<analysis::VariableSignature>(symbols->front().signature))
    {
        isEnumConstant = symbols->front().GetVariable().isEnumConstant;
    }
    return MatchAgreedSymbolType(agreedType, tokenType, !symbols->front().containerName.empty(), isEnumConstant);
}

/**
 * @brief Resolves reference kind from scope tree coordinate lookup.
 * @param[in] point Source position coordinates.
 * @param[in] referenceKinds Map from position to definition kind.
 * @param[in] defaultType Default token type if not found.
 * @return Resolved token type.
 */
uint32_t
ResolveReferenceTokenType(TSPoint point,
                          const ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& referenceKinds,
                          uint32_t defaultType)
{
    auto resolved = referenceKinds.find(PositionKey(point.row, point.column));
    if (resolved == referenceKinds.end())
    {
        return defaultType;
    }
    switch (resolved->second)
    {
    case analysis::LocalDefinitionKind::Parameter:
        return Type_Parameter;
    case analysis::LocalDefinitionKind::Field:
        return Type_Property;
    case analysis::LocalDefinitionKind::Function:
        return Type_Function;
    case analysis::LocalDefinitionKind::Method:
        return Type_Method;
    case analysis::LocalDefinitionKind::Type:
        return Type_Type;
    case analysis::LocalDefinitionKind::Constant:
        return Type_EnumMember;
    default:
        return defaultType;
    }
}

/**
 * @brief Refines token type against scoped enums, class members, and symbol table.
 * @param[in] tokenText Token identifier text.
 * @param[in] startByte Token start byte.
 * @param[in] tokenType Current token type.
 * @param[in] ctx Refinement context.
 * @return Refined token type.
 */
uint32_t RefineSyntaxContext(std::string_view tokenText, uint32_t startByte, uint32_t tokenType,
                             const TokenRefinementContext& ctx)
{
    if (auto it = ctx.scopedEnumUpgrades.find(startByte); it != ctx.scopedEnumUpgrades.end())
    {
        tokenType = it->second;
    }
    if (tokenType == Type_Variable || tokenType == Type_Function)
    {
        tokenType = RefineEnclosingClassMember(tokenText, startByte, tokenType, ctx);
    }
    if (tokenType == Type_Type || tokenType == Type_Variable || tokenType == Type_Function ||
        tokenType == Type_Namespace)
    {
        tokenType = RefineSymbolTableMatch(tokenText, tokenType, ctx);
    }
    return tokenType;
}

/**
 * @brief Refines the captured token type using scope tree, declaration site, and symbol table.
 * @param[in] node AST capture node.
 * @param[in] tokenType Initial token type.
 * @param[in] ctx Refinement context.
 * @return Final refined token type index.
 */
uint32_t RefineCapturedTokenType(TSNode node, uint32_t tokenType, const TokenRefinementContext& ctx)
{
    TSPoint startPoint = ts_node_start_point(node);
    if (tokenType == Type_Variable)
    {
        tokenType = ResolveReferenceTokenType(startPoint, ctx.referenceKinds, tokenType);
    }

    if (tokenType == Type_Type || tokenType == Type_Variable || tokenType == Type_Function ||
        tokenType == Type_Namespace)
    {
        const uint32_t startByte = ts_node_start_byte(node);
        const uint32_t endByte = ts_node_end_byte(node);
        if (startByte < endByte && endByte <= ctx.request.sourceCode.size())
        {
            std::string_view tokenText(ctx.request.sourceCode.data() + startByte, endByte - startByte);
            if (!tokenText.empty())
            {
                tokenType = RefineSyntaxContext(tokenText, startByte, tokenType, ctx);
            }
        }
    }

    return tokenType;
}

/**
 * @brief Emits boundary tokens for template angle brackets.
 * @param[in] node AST capture node.
 * @param[in] meta Token classification metadata.
 * @param[in] sourceLines Views of source code lines.
 * @param[in,out] rawTokens Token collection receiving items.
 */
void EmitTemplatePunctuationTokens(TSNode node, TokenMeta meta, const std::vector<std::string_view>& sourceLines,
                                   std::vector<RawToken>& rawTokens)
{
    TSPoint startPoint = ts_node_start_point(node);
    TSPoint endPoint = ts_node_end_point(node);

    if (startPoint.row < sourceLines.size() && startPoint.column < sourceLines[startPoint.row].size())
    {
        rawTokens.push_back(
            RawToken{startPoint.row, startPoint.column, 1, meta.tokenType, meta.tokenMod, meta.priority});
    }
    if (endPoint.column > 0 && endPoint.row < sourceLines.size() &&
        (endPoint.column - 1) < sourceLines[endPoint.row].size())
    {
        rawTokens.push_back(
            RawToken{endPoint.row, endPoint.column - 1, 1, meta.tokenType, meta.tokenMod, meta.priority});
    }
}

/**
 * @brief Emits a single token or splits multi-line tokens into line-delimited items.
 * @param[in] node AST capture node.
 * @param[in] meta Token classification metadata (type, modifier, priority).
 * @param[in] sourceLines Views of source code lines.
 * @param[in,out] rawTokens Token collection receiving items.
 */
void EmitTokenOrSplitLines(TSNode node, TokenMeta meta, const std::vector<std::string_view>& sourceLines,
                           std::vector<RawToken>& rawTokens)
{
    if (meta.tokenType == Type_TemplatePunctuation)
    {
        EmitTemplatePunctuationTokens(node, meta, sourceLines, rawTokens);
        return;
    }

    TSPoint startPoint = ts_node_start_point(node);
    TSPoint endPoint = ts_node_end_point(node);

    if (startPoint.row == endPoint.row)
    {
        if (startPoint.row < sourceLines.size())
        {
            uint32_t lineLen = static_cast<uint32_t>(sourceLines[startPoint.row].size());
            uint32_t sc = std::min(startPoint.column, lineLen);
            uint32_t ec = std::min(endPoint.column, lineLen);
            if (ec > sc)
            {
                rawTokens.push_back(
                    RawToken{startPoint.row, sc, ec - sc, meta.tokenType, meta.tokenMod, meta.priority});
            }
        }
    }
    else if (meta.tokenType != Type_Operator)
    {
        for (uint32_t r = startPoint.row; r <= endPoint.row; ++r)
        {
            if (r >= sourceLines.size())
            {
                break;
            }
            uint32_t lineLen = static_cast<uint32_t>(sourceLines[r].size());
            uint32_t sc = std::min((r == startPoint.row) ? startPoint.column : 0u, lineLen);
            uint32_t ec = std::min((r == endPoint.row) ? endPoint.column : lineLen, lineLen);
            if (ec > sc)
            {
                rawTokens.push_back(RawToken{r, sc, ec - sc, meta.tokenType, meta.tokenMod, meta.priority});
            }
        }
    }
}

/**
 * @brief Collects semantic tokens produced by executing HIGHLIGHTS_QUERY on the syntax tree.
 * @param[in] highlights Highlights query and capture rule data.
 * @param[in] ctx Refinement context.
 * @param[in] sourceLines Views of source code lines.
 * @param[in,out] rawTokens Output token collection.
 */
void CollectHighlightsTokens(const HighlightsQueryData& highlights, const TokenRefinementContext& ctx,
                             const std::vector<std::string_view>& sourceLines, std::vector<RawToken>& rawTokens)
{
    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
    ts_query_cursor_exec(cursor, highlights.query, ts_tree_root_node(ctx.request.tree));

    const auto& syms = GetGrammarSymbols();
    TSQueryMatch match;
    uint32_t captureIndex = 0;

    while (ts_query_cursor_next_capture(cursor, &match, &captureIndex))
    {
        TSNode node = match.captures[captureIndex].node;
        uint32_t captureId = match.captures[captureIndex].index;
        if (captureId >= highlights.rules.size())
        {
            continue;
        }

        const auto& rule = highlights.rules[captureId];
        if (!rule.valid)
        {
            continue;
        }

        uint32_t tokenType = rule.tokenType;
        uint32_t tokenMod = rule.tokenMod;
        int priority = rule.priority;

        if (rule.isOperatorOrPunctuation)
        {
            auto opRes = ClassifyOperatorOrPunctuation(node, ctx.request.sourceCode, syms);
            if (!opRes.has_value())
            {
                continue;
            }
            tokenType = opRes->first;
            priority = opRes->second;
        }

        tokenType = RefineCapturedTokenType(node, tokenType, ctx);
        if (tokenType == Type_Operator && IsTemplatePunctuationNode(node))
        {
            tokenType = Type_TemplatePunctuation;
        }

        EmitTokenOrSplitLines(node, TokenMeta{tokenType, tokenMod, priority}, sourceLines, rawTokens);
    }
}

/**
 * @brief Sorts and deduplicates raw tokens, removing overlapping tokens by priority.
 * @param[in,out] rawTokens Raw token vector to sort and filter.
 * @return Deduplicated and sorted vector of raw tokens.
 */
std::vector<RawToken> DeduplicateTokens(std::vector<RawToken>& rawTokens)
{
    std::sort(rawTokens.begin(), rawTokens.end(),
              [](const RawToken& a, const RawToken& b)
              {
                  if (a.line != b.line)
                      return a.line < b.line;
                  if (a.startChar != b.startChar)
                      return a.startChar < b.startChar;
                  return a.priority > b.priority;
              });

    std::vector<RawToken> filtered;
    filtered.reserve(rawTokens.size());
    uint32_t currentLine = UINT32_MAX;
    uint32_t lastEndChar = 0;

    for (const auto& tok : rawTokens)
    {
        if (tok.length == 0)
        {
            continue;
        }
        if (tok.line != currentLine)
        {
            currentLine = tok.line;
            lastEndChar = 0;
        }
        if (tok.startChar >= lastEndChar)
        {
            filtered.push_back(tok);
            lastEndChar = tok.startChar + tok.length;
        }
    }
    return filtered;
}

/**
 * @brief Narrows token collection to the requested document range using binary search.
 * @param[in,out] tokens Filtered tokens vector.
 * @param[in] range Target LSP query range.
 */
void NarrowTokensToRange(std::vector<RawToken>& tokens, const lsp::Range& range)
{
    if (tokens.empty())
    {
        return;
    }

    auto first = std::lower_bound(tokens.begin(), tokens.end(), range.start.line,
                                  [](const RawToken& tok, uint32_t line) { return tok.line < line; });

    while (first != tokens.end() && first->line == range.start.line &&
           first->startChar + first->length <= range.start.character)
    {
        ++first;
    }

    auto last = std::upper_bound(first, tokens.end(), range.end.line,
                                 [](uint32_t line, const RawToken& tok) { return line < tok.line; });

    while (last != first)
    {
        auto prev = std::prev(last);
        if (prev->line == range.end.line && prev->startChar >= range.end.character)
        {
            last = prev;
        }
        else
        {
            break;
        }
    }

    if (first >= last)
    {
        tokens.clear();
    }
    else
    {
        std::vector<RawToken> narrowed;
        narrowed.reserve(static_cast<size_t>(last - first));
        for (auto it = first; it != last; ++it)
        {
            narrowed.push_back(std::move(*it));
        }
        tokens = std::move(narrowed);
    }
}

/**
 * @brief Post-filters tokens to ensure no brackets or invalid text are emitted as Type_Operator.
 * @param[in,out] tokens Tokens vector to filter.
 * @param[in] sourceLines Views of source code lines.
 */
void FilterInvalidOperators(std::vector<RawToken>& tokens, const std::vector<std::string_view>& sourceLines)
{
    std::erase_if(tokens,
                  [&sourceLines](const RawToken& tok)
                  {
                      if (tok.tokenType == Type_Operator)
                      {
                          if (tok.line >= sourceLines.size() ||
                              tok.startChar + tok.length > sourceLines[tok.line].size())
                          {
                              return true;
                          }
                          std::string_view slice(sourceLines[tok.line].data() + tok.startChar, tok.length);
                          if (IsPunctuationOrBracket(slice) || !IsGenuineOperator(slice))
                          {
                              return true;
                          }
                      }
                      return false;
                  });
}

/**
 * @brief Encodes sorted, non-overlapping semantic tokens into LSP 5-tuple integer stream.
 * @param[in] tokens Tokens to encode.
 * @return SemanticTokens struct containing delta-encoded data stream.
 */
lsp::SemanticTokens DeltaEncodeTokens(const std::vector<RawToken>& tokens)
{
    std::vector<lsp::uint> data;
    data.reserve(tokens.size() * 5);

    uint32_t prevLine = 0;
    uint32_t prevChar = 0;

    for (const auto& tok : tokens)
    {
        uint32_t deltaLine = tok.line - prevLine;
        uint32_t deltaChar = (deltaLine == 0) ? (tok.startChar - prevChar) : tok.startChar;

        data.push_back(deltaLine);
        data.push_back(deltaChar);
        data.push_back(tok.length);
        data.push_back(tok.tokenType);
        data.push_back(tok.tokenModifiers);

        prevLine = tok.line;
        prevChar = tok.startChar;
    }

    return lsp::SemanticTokens{std::move(data)};
}
} // namespace

bool IsTemplatePunctuationNode(TSNode node) noexcept
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    TSNode grandParent = parser::GetParent(parent);
    TSSymbol parentSym = ts_node_symbol(parent);
    TSSymbol grandParentSym = !ts_node_is_null(grandParent) ? ts_node_symbol(grandParent) : 0;
    return IsTemplatePunctuation(parentSym, grandParentSym, GetGrammarSymbols());
}

const lsp::SemanticTokensLegend& GetSemanticTokensLegend()
{
    static const lsp::SemanticTokensLegend legend = {
        /* tokenTypes */ {"namespace",     "type",      "class",    "enum",     "interface",  "struct",
                          "typeParameter", "parameter", "variable", "property", "enumMember", "event",
                          "function",      "method",    "macro",    "keyword",  "modifier",   "comment",
                          "string",        "number",    "regexp",   "operator", "decorator",  "templatePunctuation"},
        /* tokenModifiers */ {"declaration", "definition", "readonly", "static", "deprecated", "abstract", "async",
                              "modification", "documentation", "defaultLibrary"}};
    return legend;
}

lsp::SemanticTokens GetSemanticTokens(const SemanticTokensRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return lsp::SemanticTokens{};
    }

    const auto& highlightsData = GetHighlightsQueryData();
    if (!highlightsData.query)
    {
        return lsp::SemanticTokens{};
    }

    const auto& syms = GetGrammarSymbols();
    std::optional<analysis::NodeIndex> localIndex;
    const analysis::NodeIndex* nodeIndex = request.nodeIndex;
    if (!nodeIndex && request.tree)
    {
        localIndex.emplace(ts_tree_root_node(request.tree));
        nodeIndex = &*localIndex;
    }

    ClassSpanIndex classSpans = ClassSpanIndex::Build(nodeIndex, request.sourceCode, syms);
    auto scopedEnumUpgrades = PrecalculateScopedEnumUpgrades(nodeIndex, request.sourceCode, request.symbolTable, syms);

    ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind> referenceKinds;
    if (request.scopeRoot)
    {
        CollectReferenceKinds(request.scopeRoot.get(), referenceKinds);
    }

    TokenRefinementContext ctx{request, referenceKinds, scopedEnumUpgrades, classSpans,
                               request.symbolTable.GetRuleIndex()};

    std::vector<RawToken> rawTokens;
    auto sourceLines = SplitLinesView(request.sourceCode);
    CollectHighlightsTokens(highlightsData, ctx, sourceLines, rawTokens);

    if (!request.excludedLineRanges.empty())
    {
        std::erase_if(rawTokens, [&request](const RawToken& tok)
                      { return angel_lsp::utils::IsLineExcluded(request.excludedLineRanges, tok.line); });
    }

    if (rawTokens.empty())
    {
        return lsp::SemanticTokens{};
    }

    std::vector<RawToken> filteredTokens = DeduplicateTokens(rawTokens);
    if (request.range.has_value())
    {
        NarrowTokensToRange(filteredTokens, *request.range);
    }
    FilterInvalidOperators(filteredTokens, sourceLines);

    return DeltaEncodeTokens(filteredTokens);
}

std::vector<lsp::SemanticTokensEdit> ComputeSemanticTokensDelta(const std::vector<lsp::uint>& previous,
                                                                const std::vector<lsp::uint>& current)
{
    if (previous == current)
    {
        return {};
    }

    if (previous.size() % 5 != 0 || current.size() % 5 != 0)
    {
        lsp::SemanticTokensEdit edit;
        edit.start = 0;
        edit.deleteCount = static_cast<lsp::uint>(previous.size());
        if (!current.empty())
        {
            edit.data = lsp::Array<lsp::uint>(current.begin(), current.end());
        }
        return {std::move(edit)};
    }

    size_t prefix = 0;
    const size_t shortest = std::min(previous.size(), current.size());
    while (prefix < shortest && previous[prefix] == current[prefix])
    {
        ++prefix;
    }
    prefix = prefix - (prefix % 5);

    size_t suffix = 0;
    while (suffix < shortest - prefix && previous[previous.size() - 1 - suffix] == current[current.size() - 1 - suffix])
    {
        ++suffix;
    }
    suffix = suffix - (suffix % 5);

    lsp::SemanticTokensEdit edit;
    edit.start = static_cast<lsp::uint>(prefix);
    edit.deleteCount = static_cast<lsp::uint>(previous.size() - prefix - suffix);

    if (current.size() - prefix - suffix > 0)
    {
        edit.data = lsp::Array<lsp::uint>(current.begin() + static_cast<std::ptrdiff_t>(prefix),
                                          current.end() - static_cast<std::ptrdiff_t>(suffix));
    }

    return {std::move(edit)};
}
} // namespace angel_lsp::features
