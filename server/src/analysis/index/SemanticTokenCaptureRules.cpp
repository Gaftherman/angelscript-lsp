#include "analysis/index/SemanticTokenFactsInternal.h"
#include "parser/QueryRegistry.h"
#include "parser/queries/BuiltQueries.h"

#include <cstring>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{

/**
 * @brief Matches special syntactic capture patterns like comments, literals, or punctuation.
 * @param[in] name Capture name.
 * @param[out] rule Output classification rule.
 * @return True if pattern matched.
 */
static bool MatchSpecialCaptureRule(std::string_view name, CaptureRule& rule)
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
static bool MatchKeywordOrConstantRule(std::string_view name, CaptureRule& rule)
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
static bool MatchCallableOrPropertyRule(std::string_view name, CaptureRule& rule)
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
static bool MatchEntityCaptureRule(std::string_view name, CaptureRule& rule)
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
static CaptureRule MakeCaptureRule(std::string_view name)
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

const HighlightsQueryData& GetHighlightsQueryData()
{
    static const HighlightsQueryData s_data = []() -> HighlightsQueryData
    {
        const TSLanguage* lang = tree_sitter_angelscript();
        uint32_t errorOffset = 0;
        TSQueryError errorType = TSQueryErrorNone;
        TSQuery* query = ts_query_new(lang, parser::queries::HIGHLIGHTS_QUERY,
                                      static_cast<uint32_t>(std::strlen(parser::queries::HIGHLIGHTS_QUERY)),
                                      &errorOffset, &errorType);
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

} // namespace angel_lsp::analysis
