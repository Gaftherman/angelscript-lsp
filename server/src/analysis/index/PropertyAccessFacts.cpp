#include "analysis/index/PropertyAccessFacts.h"

#include "analysis/index/SyntaxOperands.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

PropertyAccessFact ReadPropertyAccess(TSNode node, const NodeIndex* index, std::string_view source)
{
    PropertyAccessFact fact;
    if (ts_node_is_null(node))
    {
        return fact;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol symbol = ts_node_symbol(node);
    fact.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)};
    fact.nodeSymbol = symbol;
    if (symbol == gs.symIdentifier || symbol == gs.symScopedIdentifier)
    {
        fact.kind = PropertyAccessKind::Name;
        fact.text = parser::NodeText(node, source);
        TrimTypeWhitespace(fact.text);
        return fact;
    }
    if (symbol != gs.symMemberExpression)
    {
        return fact;
    }

    const SyntaxOperands syntax(node, index);
    TSNode object = syntax.Field(gs.fieldObject);
    TSNode member = syntax.Field(gs.fieldMember);
    if (ts_node_is_null(object))
    {
        object = syntax.FirstNamed();
        const TSNode fallbackMember = syntax.NamedAt(1);
        if (!ts_node_is_null(fallbackMember))
        {
            member = fallbackMember;
        }
    }
    if (ts_node_is_null(object) || ts_node_is_null(member))
    {
        return fact;
    }

    fact.kind = PropertyAccessKind::Member;
    fact.text = parser::NodeText(member, source);
    fact.receiver = {ts_node_start_byte(object), ts_node_end_byte(object)};
    return fact;
}

} // namespace angel_lsp::analysis
