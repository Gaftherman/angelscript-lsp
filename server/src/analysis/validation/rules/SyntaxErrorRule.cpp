/**
 * @file SyntaxErrorRule.cpp
 * @brief Implementation of SyntaxErrorRule for detecting AST syntax errors.
 * @ingroup Analysis
 */

#include "SyntaxErrorRule.h"
#include "analysis/validation/ValidationUtils.h"

namespace analysis::validation
{
    std::vector<lsDiagnostic> SyntaxErrorRule::run(const ValidationContext &ctx)
    {
        std::vector<lsDiagnostic> diags;
        if (ts_node_is_null(ctx.rootNode))
        {
            return diags;
        }

        TraverseNode(ctx.rootNode, ctx, diags);
        return diags;
    }

    void SyntaxErrorRule::TraverseNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        if (ts_node_is_null(node))
        {
            return;
        }

        if (ts_node_is_missing(node))
        {
            lsRange range = ValidationUtils::tsNodeToLsRange(node);
            std::string nodeType = ts_node_type(node) ? ts_node_type(node) : "token";
            std::string msg = "Syntax error: Missing expected '" + nodeType + "'";
            diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
        }
        else if (ts_node_is_error(node))
        {
            lsRange range = ValidationUtils::tsNodeToLsRange(node);
            std::string text = ValidationUtils::getNodeText(node, ctx.document);
            if (text.length() > 30)
            {
                text = text.substr(0, 27) + "...";
            }
            std::string msg = text.empty() ? "Syntax error: Unexpected token" : "Syntax error: Unexpected token '" + text + "'";
            diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
        }
        else
        {
            uint32_t childCount = ts_node_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TraverseNode(ts_node_child(node, i), ctx, diags);
            }
        }
    }
}
