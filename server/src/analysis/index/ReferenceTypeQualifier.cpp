#include "analysis/index/ReferenceReceiverIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{
void IndexTypeQualifier(TSNode parent, std::string_view source, const LocalReference& reference,
                        IndexedReferenceReceivers& receivers)
{
    if (receivers.qualifiers.contains(reference.startByte))
    {
        return;
    }

    const auto& grammar = parser::GetGrammarSymbols();
    const TSSymbol symbol = ts_node_symbol(parent);
    const TSNode typeNode = parser::GetParent(parent);
    if (ts_node_is_null(typeNode))
    {
        return;
    }

    if (symbol == grammar.symDatatype)
    {
        const TSSymbol typeSym = ts_node_symbol(typeNode);
        if (typeSym != grammar.symType && typeSym != grammar.symConstructCallExpression)
        {
            return;
        }
        const TSNode scopeNode = parser::FindChildWithSymbol(typeNode, grammar.symScope);
        if (ts_node_is_null(scopeNode))
        {
            return;
        }
        std::string compacted = utils::CompactScopedName(parser::NodeText(scopeNode, source));
        if (compacted == utils::k_scopeSeparator)
        {
            receivers.qualifiers.try_emplace(reference.startByte, compacted);
        }
        else if (!compacted.empty())
        {
            receivers.qualifiers.try_emplace(reference.startByte, utils::StripTrailingScopeSeparator(compacted));
        }
        return;
    }

    if (symbol == grammar.symNestedTypeName && ts_node_symbol(typeNode) == grammar.symType)
    {
        std::string prefix;
        bool hasTemplateArgs = false;
        bool hasArrayBrackets = false;
        std::string_view datatypeText;
        parser::ForEachChildNode(typeNode,
                                 [&](TSNode child)
                                 {
                                     const TSSymbol cSym = ts_node_symbol(child);
                                     if (cSym == grammar.symScope)
                                     {
                                         const std::string compacted =
                                             utils::CompactScopedName(parser::NodeText(child, source));
                                         if (compacted != utils::k_scopeSeparator)
                                         {
                                             prefix = std::string(utils::StripTrailingScopeSeparator(compacted));
                                         }
                                     }
                                     else if (cSym == grammar.symDatatype)
                                     {
                                         datatypeText = parser::NodeText(child, source);
                                     }
                                     else if (cSym == grammar.symTemplateTypeList)
                                     {
                                         hasTemplateArgs = true;
                                     }
                                     else if (cSym == grammar.tokOpenBracket)
                                     {
                                         hasArrayBrackets = true;
                                     }
                                 });
        const std::string_view ownerSegment =
            (hasArrayBrackets && !hasTemplateArgs) ? std::string_view("array") : datatypeText;
        utils::AppendScopedName(prefix, ownerSegment);

        bool done = false;
        parser::ForEachChildNode(parent,
                                 [&](TSNode child)
                                 {
                                     if (done || ts_node_symbol(child) != grammar.symIdentifier)
                                     {
                                         return;
                                     }
                                     if (ts_node_start_byte(child) == reference.startByte)
                                     {
                                         if (!prefix.empty())
                                         {
                                             receivers.qualifiers.try_emplace(reference.startByte, prefix);
                                         }
                                         done = true;
                                         return;
                                     }
                                     utils::AppendScopedName(prefix, parser::NodeText(child, source));
                                 });
    }
}
} // namespace angel_lsp::analysis
