#include "analysis/checkers/NamespaceCheckerCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a scoped identifier refers to a known namespace or type prefix.
 * @param[in] node AST node representing the scoped identifier.
 * @param[in] sourceCode Document source text.
 * @param[in] table Workspace symbol table used to resolve prefix candidates.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckScopedIdentifier(TSNode node, std::string_view sourceCode, const SymbolTable& table, DiagnosticContext& ctx)
{
    const uint32_t childCount = ts_node_child_count(node);
    if (childCount < 3)
    {
        return;
    }
    TSNode firstChild = ts_node_named_child(node, 0);
    if (ts_node_is_null(firstChild) || ts_node_symbol(firstChild) != parser::GetGrammarSymbols().symIdentifier)
    {
        return;
    }
    std::string prefix(parser::NodeText(firstChild, sourceCode));
    while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.front())))
    {
        prefix.erase(prefix.begin());
    }
    while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.back())))
    {
        prefix.pop_back();
    }
    const auto& usings = GetUsingNamespaces(ctx.request);
    if (!prefix.empty() && !IsKnownScope(prefix, node, sourceCode, table, usings, ctx.request.nodeIndex))
    {
        TSPoint startPt = ts_node_start_point(firstChild);
        TSPoint endPt = ts_node_end_point(firstChild);
        ctx.EmitAtRange({startPt.row, startPt.column, endPt.row, endPt.column}, "as-err-undefined-namespace", prefix,
                        DiagnosticSeverity::Error);
    }
}

/**
 * @brief Checks if an import declaration mistakenly specifies a body block.
 * @param[in] node AST node representing the import declaration or error node.
 * @param[in] sourceCode Document source text.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckImportBody(TSNode node, std::string_view sourceCode, DiagnosticContext& ctx)
{
    std::string nodeText(parser::NodeText(node, sourceCode));
    size_t importPos = nodeText.find("import ");
    if (importPos == std::string::npos)
    {
        return;
    }
    size_t fromPos = nodeText.find("from", importPos);
    if (fromPos != std::string::npos && nodeText.find('{', fromPos) != std::string::npos)
    {
        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt = ts_node_end_point(node);
        ctx.EmitAtRange({startPt.row, startPt.column, endPt.row, endPt.column}, "as-err-import-has-body", "import",
                        DiagnosticSeverity::Error);
    }
}

} // namespace angel_lsp::analysis
