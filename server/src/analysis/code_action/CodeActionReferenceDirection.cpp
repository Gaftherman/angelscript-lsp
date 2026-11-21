#include "analysis/code_action/CodeActionInternal.h"
#include "analysis/diagnostics/DiagnosticCodes.h"

#include <string>
#include <utility>

namespace angel_lsp::analysis::code_action
{
/**
 * @brief Creates a quick fix that inserts or replaces one reference-direction keyword.
 * @param[in] query Current code action request and locale.
 * @param[in] diagnostic Index of the diagnostic resolved by the edit.
 * @param[in] site Indexed range and spacing for the edit.
 * @param[in] direction Input or output direction to write.
 * @return A parameter edit associated with the triggering diagnostic.
 */
static CodeActionEntry MakeReferenceDirectionFix(const CodeActionQuery& query, uint32_t diagnostic,
                                                 const IndexedParameterRefSite& site, ParameterModifier direction)
{
    const bool input = direction == ParameterModifier::In;
    std::string replacement = input ? "in" : "out";
    if (site.needsSeparator)
    {
        replacement.push_back(' ');
    }
    auto title =
        input ? i18n::FormatMessage(query.i18n, "action-reference-direction-in", "Use '&in' as the reference direction")
              : i18n::FormatMessage(query.i18n, "action-reference-direction-out",
                                    "Use '&out' as the reference direction");
    CodeActionTextEdit edit{{site.editStart.line, site.editStart.character, site.editEnd.line, site.editEnd.character},
                            std::move(replacement)};
    return MakeQuickFixEntry({std::move(title), diagnostic, {std::move(edit)}, input});
}

void TryAddReferenceDirectionFixes(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (!query.parameterRefFacts || (query.config && query.config->engine.allowUnsafeReferences))
    {
        return;
    }
    for (uint32_t i = 0; i < query.diagnostics.size(); ++i)
    {
        const auto& diagnostic = query.diagnostics[i];
        if (!MatchDiagnosticCode(diagnostic, diagnostics::codes::PrimitiveInoutRefDisallowed))
        {
            continue;
        }
        const auto* site = query.parameterRefFacts->FindAt(diagnostic.range.startLine, diagnostic.range.startCharacter);
        if (!site)
        {
            continue;
        }
        entries.push_back(MakeReferenceDirectionFix(query, i, *site, ParameterModifier::In));
        if (site->allowsOutput)
        {
            entries.push_back(MakeReferenceDirectionFix(query, i, *site, ParameterModifier::Out));
        }
    }
}
} // namespace angel_lsp::analysis::code_action
