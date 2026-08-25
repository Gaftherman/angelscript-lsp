#include "features/selection_range/SelectionRangeHandler.h"

#include <memory>
#include <utility>

namespace angel_lsp::features
{
    namespace
    {
        lsp::Range ToRange(TSNode node)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            return lsp::Range{
                lsp::Position{ start.row, start.column },
                lsp::Position{ end.row, end.column }
            };
        }

        bool SameRange(const lsp::Range &a, const lsp::Range &b)
        {
            return a.start.line == b.start.line && a.start.character == b.start.character &&
                   a.end.line == b.end.line && a.end.character == b.end.character;
        }

        /**
         * @brief Assembles one position's chain, innermost first.
         *
         * A position past the end of the document, or in a document the parser could make nothing
         * of, yields the root's own range - which is what the protocol asks for: every position has
         * to be answered, in order, or the client cannot line the results up with what it sent.
         */
        lsp::SelectionRange BuildChain(TSNode root, const lsp::Position &position)
        {
            const TSPoint point{ position.line, position.character };
            TSNode node = ts_node_descendant_for_point_range(root, point, point);
            if (ts_node_is_null(node))
            {
                node = root;
            }

            std::vector<lsp::Range> ranges;
            for (TSNode current = node; !ts_node_is_null(current); current = ts_node_parent(current))
            {
                const lsp::Range range = ToRange(current);
                if (ranges.empty() || !SameRange(ranges.back(), range))
                {
                    ranges.push_back(range);
                }
            }

            if (ranges.empty())
            {
                return lsp::SelectionRange{ ToRange(root), nullptr };
            }

            // Built from the outermost inwards, because each link owns its parent.
            lsp::SelectionRange chain{ ranges.back(), nullptr };
            for (size_t i = ranges.size() - 1; i-- > 0;)
            {
                lsp::SelectionRange inner{ ranges[i], std::make_unique<lsp::SelectionRange>(std::move(chain)) };
                chain = std::move(inner);
            }
            return chain;
        }
    }

    std::vector<lsp::SelectionRange> GetSelectionRanges(const SelectionRangeRequest &request)
    {
        std::vector<lsp::SelectionRange> result;
        if (!request.tree)
        {
            return result;
        }

        const TSNode root = ts_tree_root_node(request.tree);
        if (ts_node_is_null(root))
        {
            return result;
        }

        result.reserve(request.positions.size());
        for (const auto &position : request.positions)
        {
            result.push_back(BuildChain(root, position));
        }
        return result;
    }
}
