#include "analysis/rules/DirectiveRules.h"

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "analysis/index/NodeIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis::rules
{

static void CheckModuleContext(const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (request.moduleContext.has_value() && !request.moduleContext->name.empty() &&
        !request.moduleContext->alsoClaimedBy.empty())
    {
        std::string others;
        for (const auto& name : request.moduleContext->alsoClaimedBy)
        {
            if (!others.empty())
            {
                others += ", ";
            }
            others += "'" + name + "'";
        }
        ctx.EmitAtRange({0, 0, 0, 0}, "as-hint-file-in-several-modules", {request.moduleContext->name, others},
                        DiagnosticSeverity::Hint);
    }
}

static void CheckUnsupportedDirective(const utils::UnsupportedDirective& directive,
                                      const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (directive.problem == utils::DirectiveProblem::Unrecognised)
    {
        ctx.EmitAtRange({directive.line, directive.startColumn, directive.line, directive.endColumn},
                        "as-err-unknown-directive", directive.name, DiagnosticSeverity::Error);
        return;
    }
    if (directive.problem == utils::DirectiveProblem::IncludeNotQuoted)
    {
        ctx.EmitAtRange({directive.line, directive.startColumn, directive.line, directive.endColumn},
                        "as-err-include-not-quoted", DiagnosticSeverity::Error);
        return;
    }
    if (directive.problem == utils::DirectiveProblem::SpaceAfterHash)
    {
        ctx.EmitAtRange({directive.line, directive.startColumn, directive.line, directive.endColumn},
                        "as-err-directive-space-after-hash", {directive.name, directive.name},
                        DiagnosticSeverity::Error);
        return;
    }

    ctx.EmitAtRange({directive.line, directive.startColumn, directive.line, directive.endColumn},
                    "as-warn-unsupported-directive", directive.name,
                    directive.name == "pragma" ? request.pragmaSeverity : DiagnosticSeverity::Warning);
}

void CheckDirectivesAndModules(const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    CheckModuleContext(request, ctx);
    for (const auto& directive : request.unsupportedDirectives)
    {
        CheckUnsupportedDirective(directive, request, ctx);
    }
}

void CheckMetadataSupport(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr, DiagnosticContext& ctx)
{
    if (!request.enableMetadata && indexPtr)
    {
        const auto metaNodes = indexPtr->Nodes(parser::GetGrammarSymbols().symMetadata);
        for (TSNode metaNode : metaNodes)
        {
            const TSPoint start = ts_node_start_point(metaNode);
            const TSPoint end = ts_node_end_point(metaNode);
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::MetadataDisabled,
                            DiagnosticSeverity::Warning);
        }
    }
}

void FilterSuppressedDiagnostics(std::vector<Diagnostic>& diagnostics, const SemanticAnalysisRequest& request,
                                 const NodeIndex* indexPtr)
{
    if (!request.excludedLineRanges.empty())
    {
        std::erase_if(diagnostics, [&request](const Diagnostic& d)
                      { return utils::IsLineExcluded(request.excludedLineRanges, d.range.start.line); });
    }

    if (request.enableCommentSuppressions && !request.sourceCode.empty())
    {
        const auto suppressions = ParseDiagnosticSuppressions(request.sourceCode, indexPtr);
        if (!suppressions.Empty())
        {
            std::erase_if(diagnostics,
                          [&](const Diagnostic& d) { return suppressions.IsSuppressed(d.code, d.range.start.line); });
        }
    }

    if (request.tree && ts_node_has_error(ts_tree_root_node(request.tree)) && !request.sourceCode.empty())
    {
        const auto braceBalance = parser::ComputeBraceBalance(request.sourceCode);
        if (braceBalance.HasMissingOpenBrace() && braceBalance.firstUnmatchedCloseLine >= 0)
        {
            const int cutoffLine = braceBalance.firstUnmatchedCloseLine;
            std::erase_if(diagnostics, [cutoffLine](const Diagnostic& d)
                          { return static_cast<int>(d.range.start.line) > cutoffLine && d.code != "as-syntax-error"; });
        }
    }
}

} // namespace angel_lsp::analysis::rules
