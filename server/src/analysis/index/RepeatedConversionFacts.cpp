#include "analysis/index/RepeatedConversionFacts.h"

#include "analysis/index/BinaryExpressionSyntax.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/QueryRegistry.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

static TSNode FindRootIfStatement(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    return parser::FindOutermostAncestorWithSymbol(node, gs.symIfStatement);
}

/// Reads the binary operands under the selected ladder's conditions on demand.
static std::vector<TSNode> FindOccurrencesInIfLadder(TSNode rootIf, std::string_view exprText,
                                                     std::string_view sourceCode)
{
    std::vector<TSNode> results;
    const TSQuery* query = parser::QueryRegistry::GetBinaryExpressionQuery();
    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
    if (exprText.empty() || !query || !cursor)
    {
        return results;
    }

    for (TSNode condition : analysis::CollectIfLadderConditions(rootIf, nullptr))
    {
        ts_query_cursor_exec(cursor, query, condition);
        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor, &match))
        {
            for (uint16_t i = 0; i < match.capture_count; ++i)
            {
                const auto operands = analysis::ReadBinaryOperands(match.captures[i].node, nullptr);
                for (const TSNode operand : {operands.left, operands.right})
                {
                    if (!ts_node_is_null(operand) && parser::NodeText(operand, sourceCode) == exprText)
                    {
                        results.push_back(operand);
                    }
                }
            }
        }
    }
    return results;
}

std::optional<IfLadderConversionSites> FindIfLadderConversionSites(const NodeIndex& index, std::string_view sourceCode,
                                                                   const SourceRange& diagnosticRange,
                                                                   std::string_view exprText)
{
    const TSNode root = index.Root();
    if (ts_node_is_null(root))
    {
        return std::nullopt;
    }
    const TSPoint startPt{diagnosticRange.startLine, diagnosticRange.startCharacter};
    const TSPoint endPt{diagnosticRange.endLine, diagnosticRange.endCharacter};
    const TSNode targetNode = ts_node_descendant_for_point_range(root, startPt, endPt);
    const TSNode rootIf = FindRootIfStatement(targetNode);
    if (ts_node_is_null(rootIf))
    {
        return std::nullopt;
    }

    IfLadderConversionSites sites;
    sites.rootIfLine = ts_node_start_point(rootIf).row;
    for (const TSNode node : FindOccurrencesInIfLadder(rootIf, exprText, sourceCode))
    {
        const TSPoint s = ts_node_start_point(node);
        const TSPoint e = ts_node_end_point(node);
        sites.occurrences.push_back(SourceRange{s.row, s.column, e.row, e.column});
    }
    return sites;
}

} // namespace angel_lsp::analysis
