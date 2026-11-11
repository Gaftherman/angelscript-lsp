#include "analysis/rules/DirectiveRules.h"

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleSyntaxFacts.h"
#include "parser/BraceBalance.h"
#include "spdlog/fmt/fmt.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"

#include <iterator>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{

static void CheckModuleContext(const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (request.moduleContext.has_value() && !request.moduleContext->name.empty() &&
        !request.moduleContext->alsoClaimedBy.empty())
    {
        fmt::memory_buffer others;
        for (const auto& name : request.moduleContext->alsoClaimedBy)
        {
            if (others.size() != 0)
            {
                fmt::format_to(std::back_inserter(others), ", ");
            }
            fmt::format_to(std::back_inserter(others), "'{}'", name);
        }
        ctx.EmitAtRange({0, 0, 0, 0}, "as-hint-file-in-several-modules",
                        {request.moduleContext->name, std::string_view(others.data(), others.size())},
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

/**
 * @brief Checks whether metadata syntax is used when metadata support is disabled.
 * @param[in] request Semantic analysis request containing configuration options.
 * @param[in] indexPtr NodeIndex of the document; may be null.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckMetadataSupport(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr, DiagnosticContext& ctx)
{
    if (!request.enableMetadata && indexPtr)
    {
        for (const auto& span : BuildRuleSyntaxFacts(*indexPtr).metadata)
        {
            ctx.EmitAtRange({span.startRow, span.startColumn, span.endRow, span.endColumn},
                            diagnostics::codes::MetadataDisabled, DiagnosticSeverity::Warning);
        }
    }
}

/**
 * @brief Filters diagnostics by excluded line ranges, inline comments, and syntax recovery.
 * @param[in,out] diagnostics Vector of emitted diagnostics to filter in-place.
 * @param[in] request Semantic analysis request.
 * @param[in] indexPtr NodeIndex of the document.
 */
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

    if (indexPtr && indexPtr->HasSyntaxErrors() && !request.sourceCode.empty())
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
