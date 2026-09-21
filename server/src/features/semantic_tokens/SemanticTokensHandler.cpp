#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "analysis/NodeIndex.h"
#include "analysis/rules/RuleIndex.h"
#include "parser/GrammarNames.h"
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

/**
 * @brief Cached Tree-Sitter grammar symbols used across tokenization passes.
 */
struct GrammarSymbols
{
    TSSymbol symTemplateTypeList = 0;
    TSSymbol symCastExpression = 0;
    TSSymbol symTemplateParameterList = 0;
    TSSymbol symBinaryExpression = 0;
    TSSymbol symAssignmentExpression = 0;
    TSSymbol symUnaryExpression = 0;
    TSSymbol symPostfixExpression = 0;
    TSSymbol symStatementBlock = 0;
    TSSymbol symLambdaExpression = 0;
    TSSymbol symParameter = 0;
    TSSymbol symInterfaceMethod = 0;
    TSSymbol symFuncDeclaration = 0;
    TSSymbol symVariableDeclaration = 0;
    TSSymbol symClassBody = 0;
    TSSymbol symInterfaceBody = 0;
    TSSymbol symEnumDeclaration = 0;
    TSSymbol symEnumMember = 0;
    TSSymbol symScopedIdentifier = 0;
    TSSymbol symClassDeclaration = 0;
    TSSymbol symMemberExpression = 0;
    TSSymbol symIdentifier = 0;
    TSSymbol symLambdaParameterList = 0;
};

/**
 * @brief Initializes and returns statically cached Tree-Sitter grammar symbols.
 * @return Reference to cached grammar symbols.
 */
const GrammarSymbols& GetGrammarSymbols()
{
    static const GrammarSymbols s_symbols = []()
    {
        const TSLanguage* lang = tree_sitter_angelscript();
        auto symFor = [lang](std::string_view name) -> TSSymbol
        { return ts_language_symbol_for_name(lang, name.data(), static_cast<uint32_t>(name.size()), true); };
        GrammarSymbols gs;
        gs.symTemplateTypeList = symFor(parser::nodes::TemplateTypeList);
        gs.symCastExpression = symFor(parser::nodes::CastExpression);
        gs.symTemplateParameterList = symFor(parser::nodes::TemplateParameterList);
        gs.symBinaryExpression = symFor(parser::nodes::BinaryExpression);
        gs.symAssignmentExpression = symFor(parser::nodes::AssignmentExpression);
        gs.symUnaryExpression = symFor(parser::nodes::UnaryExpression);
        gs.symPostfixExpression = symFor(parser::nodes::PostfixExpression);
        gs.symStatementBlock = symFor(parser::nodes::StatementBlock);
        gs.symLambdaExpression = symFor(parser::nodes::LambdaExpression);
        gs.symParameter = symFor(parser::nodes::Parameter);
        gs.symInterfaceMethod = symFor(parser::nodes::InterfaceMethod);
        gs.symFuncDeclaration = symFor(parser::nodes::FuncDeclaration);
        gs.symVariableDeclaration = symFor(parser::nodes::VariableDeclaration);
        gs.symClassBody = symFor(parser::nodes::ClassBody);
        gs.symInterfaceBody = symFor(parser::nodes::InterfaceBody);
        gs.symEnumDeclaration = symFor(parser::nodes::EnumDeclaration);
        gs.symEnumMember = symFor(parser::nodes::EnumMember);
        gs.symScopedIdentifier = symFor(parser::nodes::ScopedIdentifier);
        gs.symClassDeclaration = symFor(parser::nodes::ClassDeclaration);
        gs.symMemberExpression = symFor(parser::nodes::MemberExpression);
        gs.symIdentifier = symFor(parser::nodes::Identifier);
        gs.symLambdaParameterList = symFor(parser::nodes::LambdaParameterList);
        return gs;
    }();
    return s_symbols;
}

/**
 * @brief Checks if an AST node is a `<` or `>` bracket in a template type list or cast.
 * @param[in] node AST node to check.
 * @return True if node is template punctuation.
 */
[[nodiscard]] inline bool IsTemplatePunctuationNode(TSNode node) noexcept
{
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }

    const auto& syms = GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == syms.symTemplateTypeList || parentSym == syms.symCastExpression ||
        parentSym == syms.symTemplateParameterList)
    {
        return true;
    }

    TSNode grandParent = ts_node_parent(parent);
    if (!ts_node_is_null(grandParent))
    {
        const TSSymbol grandParentSym = ts_node_symbol(grandParent);
        if (grandParentSym == syms.symTemplateTypeList || grandParentSym == syms.symCastExpression ||
            grandParentSym == syms.symTemplateParameterList)
        {
            if (parentSym != syms.symBinaryExpression && parentSym != syms.symAssignmentExpression &&
                parentSym != syms.symUnaryExpression && parentSym != syms.symPostfixExpression)
            {
                return true;
            }
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
 * @brief Checks if a node corresponds to the "name" field of its parent.
 * @param[in] node Identifier node.
 * @param[in] parent Enclosing declaration node.
 * @return True if node matches the name field.
 */
bool IsNodeNameField(TSNode node, TSNode parent) noexcept
{
    if (ts_node_is_null(parent))
    {
        return false;
    }
    TSNode nameChild = parser::GetChildByField(parent, parser::fields::Name);
    if (ts_node_is_null(nameChild))
    {
        return false;
    }
    if (ts_node_eq(nameChild, node))
    {
        return true;
    }
    uint32_t nameStart = ts_node_start_byte(nameChild);
    uint32_t nameEnd = ts_node_end_byte(nameChild);
    uint32_t nodeStart = ts_node_start_byte(node);
    uint32_t nodeEnd = ts_node_end_byte(node);
    if (nameStart == nameEnd || nodeStart == nodeEnd)
    {
        return false;
    }
    return (nameStart <= nodeStart && nodeEnd <= nameEnd) || (nodeStart <= nameStart && nameEnd <= nodeEnd);
}

/**
 * @brief Refines func_declaration token type based on enclosing container.
 * @param[in] node Identifier node.
 * @param[in] curr func_declaration node.
 * @param[in] syms Grammar symbol table.
 * @return Optional refined token type (Type_Method or nullopt).
 */
std::optional<uint32_t> RefineMethodOrFunction(TSNode node, TSNode curr, const GrammarSymbols& syms) noexcept
{
    if (!IsNodeNameField(node, curr))
    {
        return std::nullopt;
    }
    for (TSNode anc = ts_node_parent(curr); !ts_node_is_null(anc); anc = ts_node_parent(anc))
    {
        const TSSymbol ancSym = ts_node_symbol(anc);
        if (ancSym == syms.symClassBody || ancSym == syms.symInterfaceBody)
        {
            return Type_Method;
        }
    }
    return std::nullopt;
}

/**
 * @brief Refines variable_declaration token type when declared inside a class body.
 * @param[in] curr variable_declaration node.
 * @param[in] syms Grammar symbol table.
 * @return Optional refined token type (Type_Property or nullopt).
 */
std::optional<uint32_t> RefineClassProperty(TSNode curr, const GrammarSymbols& syms) noexcept
{
    for (TSNode anc = ts_node_parent(curr); !ts_node_is_null(anc); anc = ts_node_parent(anc))
    {
        const TSSymbol ancSym = ts_node_symbol(anc);
        if (ancSym == syms.symStatementBlock || ancSym == syms.symFuncDeclaration)
        {
            break;
        }
        if (ancSym == syms.symClassBody)
        {
            return Type_Property;
        }
    }
    return std::nullopt;
}

/**
 * @brief Refines enum members vs enum's own name.
 * @param[in] node Identifier node.
 * @param[in] curr enum_declaration or enum_member node.
 * @param[in] syms Grammar symbol table.
 * @return Optional refined token type (Type_EnumMember or nullopt).
 */
std::optional<uint32_t> RefineEnumDeclarationOrMember(TSNode node, TSNode curr, const GrammarSymbols& syms) noexcept
{
    bool isEnumOwnName = false;
    for (TSNode anc = curr; !ts_node_is_null(anc); anc = ts_node_parent(anc))
    {
        if (ts_node_symbol(anc) == syms.symEnumDeclaration)
        {
            if (IsNodeNameField(node, anc))
            {
                isEnumOwnName = true;
            }
            break;
        }
    }
    if (!isEnumOwnName)
    {
        return Type_EnumMember;
    }
    return std::nullopt;
}

/**
 * @brief Result from inspecting a syntax ancestor for token refinement.
 */
struct AncestorRefinementResult
{
    bool terminal = false;
    std::optional<uint32_t> refinedType;
};

/**
 * @brief Inspects a single ancestor node to refine an identifier token.
 * @param[in] node Target identifier node.
 * @param[in] directParent Direct parent of identifier node.
 * @param[in] curr Current ancestor node being tested.
 * @param[in] syms Grammar symbol table.
 * @return AncestorRefinementResult indication.
 */
AncestorRefinementResult InspectAncestorNode(TSNode node, TSNode directParent, TSNode curr,
                                             const GrammarSymbols& syms) noexcept
{
    const TSSymbol currSym = ts_node_symbol(curr);
    if (currSym == syms.symStatementBlock || currSym == syms.symLambdaExpression)
    {
        return {true, std::nullopt};
    }
    if ((currSym == syms.symParameter || currSym == syms.symInterfaceMethod) &&
        (IsNodeNameField(node, curr) || ts_node_eq(directParent, curr)))
    {
        return {true, (currSym == syms.symParameter) ? Type_Parameter : Type_Method};
    }
    if (currSym == syms.symFuncDeclaration)
    {
        auto refined = RefineMethodOrFunction(node, curr, syms);
        return {true, refined};
    }
    if (currSym == syms.symVariableDeclaration)
    {
        if (auto prop = RefineClassProperty(curr, syms))
        {
            return {false, prop};
        }
    }
    if (currSym == syms.symEnumDeclaration || currSym == syms.symEnumMember)
    {
        auto member = RefineEnumDeclarationOrMember(node, curr, syms);
        return {true, member};
    }
    return {false, std::nullopt};
}

/**
 * @brief Upgrades coarse token types for identifier declarations by walking ancestors.
 * @param[in] node Identifier AST node.
 * @param[in] tokenType Current coarse token type.
 * @return Refined token type index.
 */
[[nodiscard]] inline uint32_t RefineDeclarationTokenType(TSNode node, uint32_t tokenType) noexcept
{
    if (tokenType != Type_Variable && tokenType != Type_Function)
    {
        return tokenType;
    }
    if (ts_node_is_null(node))
    {
        return tokenType;
    }

    TSNode directParent = ts_node_parent(node);
    if (ts_node_is_null(directParent))
    {
        return tokenType;
    }

    const auto& syms = GetGrammarSymbols();
    TSNode curr = directParent;

    for (int level = 0; level < 8 && !ts_node_is_null(curr); ++level, curr = ts_node_parent(curr))
    {
        auto res = InspectAncestorNode(node, directParent, curr, syms);
        if (res.refinedType.has_value())
        {
            return *res.refinedType;
        }
        if (res.terminal)
        {
            return tokenType;
        }
    }
    return tokenType;
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
    if (name == "function")
    {
        rule.tokenType = Type_Function;
        rule.tokenMod = Mod_Declaration;
        rule.priority = 6;
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
    if (name == "variable" || name == "variable.parameter")
    {
        rule.tokenType = (name == "variable.parameter") ? Type_Parameter : Type_Variable;
        rule.priority = (name == "variable.parameter") ? 4 : 3;
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
    std::string_view name;
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
                    index.m_spans.push_back(ClassSpan{ts_node_start_byte(classDecl), ts_node_end_byte(classDecl),
                                                      std::string_view(sourceCode.data() + cStart, cEnd - cStart), -1});
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
 * @brief Records variable declarator identifiers as properties in class bodies.
 * @param[in] member Variable declaration AST node.
 * @param[in] syms Grammar symbols.
 * @param[in,out] out Refinement map from start byte to Type_Property.
 */
void RecordVariableDeclaratorRefinements(TSNode member, const GrammarSymbols& syms,
                                         ankerl::unordered_dense::map<uint32_t, uint32_t>& out)
{
    const uint32_t varChildCount = ts_node_child_count(member);
    for (uint32_t j = 0; j < varChildCount; ++j)
    {
        TSNode child = ts_node_child(member, j);
        if (ts_node_symbol(child) == syms.symIdentifier)
        {
            out[ts_node_start_byte(child)] = Type_Property;
        }
        else
        {
            TSNode nameChild = parser::GetChildByField(child, parser::fields::Name);
            if (!ts_node_is_null(nameChild))
            {
                out[ts_node_start_byte(nameChild)] = Type_Property;
            }
            else
            {
                const uint32_t subCount = ts_node_child_count(child);
                for (uint32_t k = 0; k < subCount; ++k)
                {
                    TSNode subChild = ts_node_child(child, k);
                    if (ts_node_symbol(subChild) == syms.symIdentifier)
                    {
                        out[ts_node_start_byte(subChild)] = Type_Property;
                    }
                }
            }
        }
    }
}

/**
 * @brief Records parameter and interface method refinements into the map.
 * @param[in] nodeIndex Pre-built AST index.
 * @param[in] syms Grammar symbols.
 * @param[in,out] out Output refinement map.
 */
void RecordParamAndMethodRefinements(const analysis::NodeIndex& nodeIndex, const GrammarSymbols& syms,
                                     ankerl::unordered_dense::map<uint32_t, uint32_t>& out)
{
    for (TSNode paramNode : nodeIndex.Nodes(syms.symParameter))
    {
        TSNode nameChild = parser::GetChildByField(paramNode, parser::fields::Name);
        if (!ts_node_is_null(nameChild))
        {
            out[ts_node_start_byte(nameChild)] = Type_Parameter;
        }
    }
    for (TSNode methodNode : nodeIndex.Nodes(syms.symInterfaceMethod))
    {
        TSNode nameChild = parser::GetChildByField(methodNode, parser::fields::Name);
        if (!ts_node_is_null(nameChild))
        {
            out[ts_node_start_byte(nameChild)] = Type_Method;
        }
    }
}

/**
 * @brief Records class and interface body member refinements into the map.
 * @param[in] nodeIndex Pre-built AST index.
 * @param[in] syms Grammar symbols.
 * @param[in,out] out Output refinement map.
 */
void RecordBodyMemberRefinements(const analysis::NodeIndex& nodeIndex, const GrammarSymbols& syms,
                                 ankerl::unordered_dense::map<uint32_t, uint32_t>& out)
{
    for (TSNode classBody : nodeIndex.Nodes(syms.symClassBody))
    {
        const uint32_t memberCount = ts_node_child_count(classBody);
        for (uint32_t i = 0; i < memberCount; ++i)
        {
            TSNode member = ts_node_child(classBody, i);
            const TSSymbol memberSym = ts_node_symbol(member);
            if (memberSym == syms.symFuncDeclaration)
            {
                TSNode nameChild = parser::GetChildByField(member, parser::fields::Name);
                if (!ts_node_is_null(nameChild))
                {
                    out[ts_node_start_byte(nameChild)] = Type_Method;
                }
            }
            else if (memberSym == syms.symVariableDeclaration)
            {
                RecordVariableDeclaratorRefinements(member, syms, out);
            }
        }
    }

    for (TSNode ifaceBody : nodeIndex.Nodes(syms.symInterfaceBody))
    {
        const uint32_t memberCount = ts_node_child_count(ifaceBody);
        for (uint32_t i = 0; i < memberCount; ++i)
        {
            TSNode member = ts_node_child(ifaceBody, i);
            const TSSymbol memberSym = ts_node_symbol(member);
            if (memberSym == syms.symFuncDeclaration || memberSym == syms.symInterfaceMethod)
            {
                TSNode nameChild = parser::GetChildByField(member, parser::fields::Name);
                if (!ts_node_is_null(nameChild))
                {
                    out[ts_node_start_byte(nameChild)] = Type_Method;
                }
            }
        }
    }
}

/**
 * @brief Records enum member refinements into the map.
 * @param[in] nodeIndex Pre-built AST index.
 * @param[in] syms Grammar symbols.
 * @param[in,out] out Output refinement map.
 */
void RecordEnumMemberRefinements(const analysis::NodeIndex& nodeIndex, const GrammarSymbols& syms,
                                 ankerl::unordered_dense::map<uint32_t, uint32_t>& out)
{
    for (TSNode enumMember : nodeIndex.Nodes(syms.symEnumMember))
    {
        TSNode nameChild = parser::GetChildByField(enumMember, parser::fields::Name);
        if (!ts_node_is_null(nameChild))
        {
            out[ts_node_start_byte(nameChild)] = Type_EnumMember;
        }
        else
        {
            const uint32_t childCount = ts_node_child_count(enumMember);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_child(enumMember, i);
                if (ts_node_symbol(child) == syms.symIdentifier)
                {
                    out[ts_node_start_byte(child)] = Type_EnumMember;
                }
            }
        }
    }
}

/**
 * @brief Pre-calculates declaration refinements across AST to avoid ancestor walks.
 * @param[in] nodeIndex Pre-indexed AST, or nullptr.
 * @param[in] syms Grammar symbols.
 * @return Map from start byte to refined token type.
 */
ankerl::unordered_dense::map<uint32_t, uint32_t>
PrecalculateDeclarationRefinements(const analysis::NodeIndex* nodeIndex, const GrammarSymbols& syms)
{
    ankerl::unordered_dense::map<uint32_t, uint32_t> refinedDecl;
    if (!nodeIndex)
    {
        return refinedDecl;
    }
    RecordParamAndMethodRefinements(*nodeIndex, syms, refinedDecl);
    RecordBodyMemberRefinements(*nodeIndex, syms, refinedDecl);
    RecordEnumMemberRefinements(*nodeIndex, syms, refinedDecl);
    return refinedDecl;
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
        uint32_t namedCount = ts_node_named_child_count(scopedNode);
        TSNode leftNode = TSNode{};
        TSNode rightNode = TSNode{};
        if (namedCount >= 2)
        {
            leftNode = ts_node_named_child(scopedNode, 0);
            rightNode = ts_node_named_child(scopedNode, namedCount - 1);
        }
        else
        {
            uint32_t allCount = ts_node_child_count(scopedNode);
            if (allCount >= 2)
            {
                leftNode = ts_node_child(scopedNode, 0);
                rightNode = ts_node_child(scopedNode, allCount - 1);
            }
        }

        if (!ts_node_is_null(leftNode) && !ts_node_is_null(rightNode))
        {
            uint32_t leftStart = ts_node_start_byte(leftNode);
            uint32_t leftEnd = ts_node_end_byte(leftNode);
            if (leftStart < leftEnd && leftEnd <= sourceCode.size())
            {
                std::string_view leftText(sourceCode.data() + leftStart, leftEnd - leftStart);
                const auto leftSymbols = symbolTable.FindSymbolsPtr(leftText);
                if (leftSymbols && !leftSymbols->empty())
                {
                    bool allEnum = std::all_of(leftSymbols->begin(), leftSymbols->end(), [](const analysis::Symbol& sym)
                                               { return sym.type == analysis::SymbolType::Enum; });
                    if (allEnum)
                    {
                        upgrades[leftStart] = Type_Enum;
                        uint32_t rightStart = ts_node_start_byte(rightNode);
                        upgrades[rightStart] = Type_EnumMember;
                    }
                }
            }
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
    const ankerl::unordered_dense::map<uint32_t, uint32_t>& refinedDeclByStartByte;
    const ankerl::unordered_dense::map<uint32_t, uint32_t>& scopedEnumUpgrades;
    const ClassSpanIndex& classSpans;
    std::shared_ptr<const analysis::rules::RuleIndex> ruleIndex;
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
    char keyBuf[256];
    std::string_view qualifiedKey;
    std::string heapKey;
    const size_t keyLen = className.size() + 2 + tokenText.size();
    if (keyLen < sizeof(keyBuf))
    {
        memcpy(keyBuf, className.data(), className.size());
        keyBuf[className.size()] = ':';
        keyBuf[className.size() + 1] = ':';
        memcpy(keyBuf + className.size() + 2, tokenText.data(), tokenText.size());
        qualifiedKey = std::string_view(keyBuf, keyLen);
    }
    else
    {
        heapKey.reserve(keyLen);
        heapKey.append(className);
        heapKey.append("::");
        heapKey.append(tokenText);
        qualifiedKey = heapKey;
    }

    const auto& typeMembers = ctx.ruleIndex->Members(className);
    if (typeMembers.memberKeySet.contains(qualifiedKey))
    {
        if (tokenType == Type_Variable)
        {
            return Type_Property;
        }
        if (tokenType == Type_Function)
        {
            return Type_Method;
        }
    }
    return tokenType;
}

/**
 * @brief Matches agreed SymbolType with current coarse token type.
 * @param[in] agreedType Consensus symbol type.
 * @param[in] tokenType Current coarse token type.
 * @param[in] hasContainer True if symbol belongs to a container.
 * @return Refined token type.
 */
uint32_t MatchAgreedSymbolType(analysis::SymbolType agreedType, uint32_t tokenType, bool hasContainer)
{
    if (tokenType == Type_Type)
    {
        if (agreedType == analysis::SymbolType::Class)
            return Type_Class;
        if (agreedType == analysis::SymbolType::Interface)
            return Type_Interface;
        if (agreedType == analysis::SymbolType::Enum)
            return Type_Enum;
    }
    if (agreedType == analysis::SymbolType::Function && tokenType == Type_Function && hasContainer)
    {
        return Type_Method;
    }
    if (agreedType == analysis::SymbolType::Variable && tokenType == Type_Variable && hasContainer)
    {
        return Type_Property;
    }
    return tokenType;
}

/**
 * @brief Matches an identifier with symbolTable declarations to upgrade coarse tokens.
 * @param[in] tokenText Identifier text.
 * @param[in] tokenType Current token type.
 * @param[in] symbolTable Global symbol table.
 * @return Refined token type.
 */
uint32_t RefineSymbolTableMatch(std::string_view tokenText, uint32_t tokenType,
                                const analysis::SymbolTable& symbolTable)
{
    const auto symbols = symbolTable.FindSymbolsPtr(tokenText);
    if (!symbols || symbols->empty())
    {
        return tokenType;
    }
    const analysis::SymbolType agreedType = symbols->front().type;
    bool allSymbolsMatch = std::all_of(symbols->begin(), symbols->end(),
                                       [agreedType](const analysis::Symbol& s) { return s.type == agreedType; });
    if (!allSymbolsMatch)
    {
        return tokenType;
    }
    return MatchAgreedSymbolType(agreedType, tokenType, !symbols->front().containerName.empty());
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
    if (tokenType == Type_Type || tokenType == Type_Variable || tokenType == Type_Function)
    {
        tokenType = RefineSymbolTableMatch(tokenText, tokenType, ctx.request.symbolTable);
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

    if (tokenType == Type_Variable || tokenType == Type_Function)
    {
        const uint32_t sb = ts_node_start_byte(node);
        if (auto it = ctx.refinedDeclByStartByte.find(sb); it != ctx.refinedDeclByStartByte.end())
        {
            tokenType = it->second;
        }
        else if (!ctx.request.nodeIndex)
        {
            tokenType = RefineDeclarationTokenType(node, tokenType);
        }
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
 * @brief Emits a single token or splits multi-line tokens into line-delimited items.
 * @param[in] node AST capture node.
 * @param[in] meta Token classification metadata (type, modifier, priority).
 * @param[in] sourceLines Views of source code lines.
 * @param[in,out] rawTokens Token collection receiving items.
 */
void EmitTokenOrSplitLines(TSNode node, TokenMeta meta, const std::vector<std::string_view>& sourceLines,
                           std::vector<RawToken>& rawTokens)
{
    TSPoint startPoint = ts_node_start_point(node);
    TSPoint endPoint = ts_node_end_point(node);

    if (meta.tokenType == Type_TemplatePunctuation)
    {
        rawTokens.push_back(
            RawToken{startPoint.row, startPoint.column, 1, meta.tokenType, meta.tokenMod, meta.priority});
        if (endPoint.column > 0)
        {
            rawTokens.push_back(
                RawToken{endPoint.row, endPoint.column - 1, 1, meta.tokenType, meta.tokenMod, meta.priority});
        }
        return;
    }

    if (startPoint.row == endPoint.row)
    {
        if (endPoint.column > startPoint.column)
        {
            rawTokens.push_back(RawToken{startPoint.row, startPoint.column, endPoint.column - startPoint.column,
                                         meta.tokenType, meta.tokenMod, meta.priority});
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
            uint32_t sc = (r == startPoint.row) ? startPoint.column : 0;
            uint32_t ec = (r == endPoint.row) ? endPoint.column : lineLen;
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
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, highlights.query, ts_tree_root_node(ctx.request.tree));

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
            const uint32_t sb = ts_node_start_byte(node);
            const uint32_t eb = ts_node_end_byte(node);
            if (sb < eb && eb <= ctx.request.sourceCode.size())
            {
                std::string_view text(ctx.request.sourceCode.data() + sb, eb - sb);
                if (IsPunctuationOrBracket(text) || !IsGenuineOperator(text))
                {
                    continue;
                }
                tokenType = Type_Operator;
                priority = 4;
            }
            else
            {
                continue;
            }
        }

        tokenType = RefineCapturedTokenType(node, tokenType, ctx);
        if (tokenType == Type_Operator && IsTemplatePunctuationNode(node))
        {
            tokenType = Type_TemplatePunctuation;
        }

        EmitTokenOrSplitLines(node, TokenMeta{tokenType, tokenMod, priority}, sourceLines, rawTokens);
    }

    ts_query_cursor_delete(cursor);
}

/**
 * @brief Processes member expression nodes to emit property tokens.
 * @param[in] curr Member expression AST node.
 * @param[in] syms Grammar symbols.
 * @param[in,out] existingStarts Coordinate set of already-started tokens.
 * @param[in,out] rawTokens Token collection receiving items.
 */
void ProcessMemberExpressionNode(TSNode curr, const GrammarSymbols& syms,
                                 ankerl::unordered_dense::set<uint64_t>& existingStarts,
                                 std::vector<RawToken>& rawTokens)
{
    TSNode memberNode = parser::GetChildByField(curr, parser::fields::Member);
    if (!ts_node_is_null(memberNode) && ts_node_symbol(memberNode) == syms.symIdentifier)
    {
        TSPoint mStart = ts_node_start_point(memberNode);
        TSPoint mEnd = ts_node_end_point(memberNode);
        uint64_t posKey = PositionKey(mStart.row, mStart.column);
        if (!existingStarts.contains(posKey) && mStart.row == mEnd.row && mEnd.column > mStart.column)
        {
            rawTokens.push_back(RawToken{mStart.row, mStart.column, mEnd.column - mStart.column, Type_Property, 0, 3});
            existingStarts.insert(posKey);
        }
    }
}

/**
 * @brief Processes lambda parameter list nodes to emit parameter tokens.
 * @param[in] curr Lambda parameter list AST node.
 * @param[in] syms Grammar symbols.
 * @param[in,out] existingStarts Coordinate set of already-started tokens.
 * @param[in,out] rawTokens Token collection receiving items.
 */
void ProcessLambdaParameterListNode(TSNode curr, const GrammarSymbols& syms,
                                    ankerl::unordered_dense::set<uint64_t>& existingStarts,
                                    std::vector<RawToken>& rawTokens)
{
    const uint32_t count = ts_node_named_child_count(curr);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(curr, i);
        if (!ts_node_is_null(child) && ts_node_symbol(child) == syms.symIdentifier)
        {
            TSPoint start = ts_node_start_point(child);
            TSPoint end = ts_node_end_point(child);
            uint64_t posKey = PositionKey(start.row, start.column);
            if (!existingStarts.contains(posKey) && start.row == end.row && end.column > start.column)
            {
                rawTokens.push_back(
                    RawToken{start.row, start.column, end.column - start.column, Type_Parameter, Mod_Declaration, 3});
                existingStarts.insert(posKey);
            }
        }
    }
}

/**
 * @brief Scans member expressions and lambda parameter lists via precomputed NodeIndex.
 * @param[in] nodeIndex Precomputed AST node index.
 * @param[in] syms Grammar symbols.
 * @param[in,out] existingStarts Coordinate set of already-started tokens.
 * @param[in,out] rawTokens Token collection receiving items.
 */
void CollectMemberAndLambdaFromIndex(const analysis::NodeIndex* nodeIndex, const GrammarSymbols& syms,
                                     ankerl::unordered_dense::set<uint64_t>& existingStarts,
                                     std::vector<RawToken>& rawTokens)
{
    for (TSNode curr : nodeIndex->Nodes(syms.symMemberExpression))
    {
        ProcessMemberExpressionNode(curr, syms, existingStarts, rawTokens);
    }
    for (TSNode curr : nodeIndex->Nodes(syms.symLambdaParameterList))
    {
        ProcessLambdaParameterListNode(curr, syms, existingStarts, rawTokens);
    }
}

/**
 * @brief Scans member expressions and lambda parameter lists via iterative stack traversal.
 * @param[in] rootNode Document root AST node.
 * @param[in] syms Grammar symbols.
 * @param[in,out] existingStarts Coordinate set of already-started tokens.
 * @param[in,out] rawTokens Token collection receiving items.
 */
void CollectMemberAndLambdaFromTree(TSNode rootNode, const GrammarSymbols& syms,
                                    ankerl::unordered_dense::set<uint64_t>& existingStarts,
                                    std::vector<RawToken>& rawTokens)
{
    std::vector<TSNode> stack{rootNode};
    while (!stack.empty())
    {
        TSNode curr = stack.back();
        stack.pop_back();
        if (ts_node_is_null(curr))
        {
            continue;
        }
        TSSymbol s = ts_node_symbol(curr);
        if (s == syms.symMemberExpression)
        {
            ProcessMemberExpressionNode(curr, syms, existingStarts, rawTokens);
        }
        else if (s == syms.symLambdaParameterList)
        {
            ProcessLambdaParameterListNode(curr, syms, existingStarts, rawTokens);
        }
        uint32_t cc = ts_node_child_count(curr);
        for (uint32_t i = 0; i < cc; ++i)
        {
            stack.push_back(ts_node_child(curr, i));
        }
    }
}

/**
 * @brief Processes member expressions and lambda parameter lists to emit property and parameter tokens.
 * @param[in] rootNode Document root node.
 * @param[in] nodeIndex Pre-built AST index, or nullptr.
 * @param[in] syms Grammar symbols.
 * @param[in,out] rawTokens Token collection receiving items.
 */
void CollectMemberAndLambdaTokens(TSNode rootNode, const analysis::NodeIndex* nodeIndex, const GrammarSymbols& syms,
                                  std::vector<RawToken>& rawTokens)
{
    ankerl::unordered_dense::set<uint64_t> existingStarts;
    existingStarts.reserve(rawTokens.size());
    for (const auto& tok : rawTokens)
    {
        existingStarts.insert(PositionKey(tok.line, tok.startChar));
    }
    if (nodeIndex)
    {
        CollectMemberAndLambdaFromIndex(nodeIndex, syms, existingStarts, rawTokens);
    }
    else
    {
        CollectMemberAndLambdaFromTree(rootNode, syms, existingStarts, rawTokens);
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
    auto refinedDecl = PrecalculateDeclarationRefinements(nodeIndex, syms);
    auto scopedEnumUpgrades = PrecalculateScopedEnumUpgrades(nodeIndex, request.sourceCode, request.symbolTable, syms);

    ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind> referenceKinds;
    if (request.scopeRoot)
    {
        CollectReferenceKinds(request.scopeRoot.get(), referenceKinds);
    }

    TokenRefinementContext ctx{
        request, referenceKinds, refinedDecl, scopedEnumUpgrades, classSpans, request.symbolTable.GetRuleIndex()};

    std::vector<RawToken> rawTokens;
    auto sourceLines = SplitLinesView(request.sourceCode);
    CollectHighlightsTokens(highlightsData, ctx, sourceLines, rawTokens);
    CollectMemberAndLambdaTokens(ts_tree_root_node(request.tree), nodeIndex, syms, rawTokens);

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
