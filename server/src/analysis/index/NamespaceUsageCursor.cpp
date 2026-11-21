#include "analysis/index/NamespaceUsageFacts.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
std::optional<ScopedNameAtCursor> FindScopedNameAtCursor(const NodeIndex& index, std::string_view source,
                                                         uint32_t cursorByte) noexcept
{
    const auto identifiers = index.Nodes(parser::GetGrammarSymbols().symIdentifier);
    const auto after = std::ranges::upper_bound(identifiers, cursorByte, {}, ts_node_start_byte);
    if (after == identifiers.begin())
    {
        return std::nullopt;
    }

    // The symbol bucket contains non-null nodes in source order.
    const auto selected = after - 1;
    const auto selectedStart = ts_node_start_byte(*selected);
    const auto selectedEnd = ts_node_end_byte(*selected);
    if (selectedStart == selectedEnd || cursorByte > selectedEnd || selectedEnd > source.size())
    {
        return std::nullopt;
    }

    const auto joinedByScopeSeparator = [source](TSNode left, TSNode right)
    {
        const auto leftEnd = ts_node_end_byte(left);
        const auto rightStart = ts_node_start_byte(right);
        return leftEnd <= rightStart && rightStart <= source.size() &&
               source.substr(leftEnd, rightStart - leftEnd) == utils::k_scopeSeparator;
    };
    auto first = selected;
    while (first != identifiers.begin() && joinedByScopeSeparator(*(first - 1), *first))
    {
        --first;
    }
    auto last = selected;
    while (last + 1 != identifiers.end() && joinedByScopeSeparator(*last, *(last + 1)))
    {
        ++last;
    }

    const auto start = ts_node_start_byte(*first);
    return ScopedNameAtCursor{source.substr(start, selectedEnd - start),
                              source.substr(start, ts_node_end_byte(*last) - start)};
}
} // namespace angel_lsp::analysis
