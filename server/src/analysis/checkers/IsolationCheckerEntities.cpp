#include "analysis/checkers/IsolationCheckerCommon.h"
#include "analysis/index/IsolationSiteFacts.h"

namespace angel_lsp::analysis
{

void CheckSharedEntityEligibility(const IsolationSiteFacts& facts, DiagnosticContext& ctx)
{
    for (const auto& fact : facts.sharedModifiers)
    {
        if (!fact.isValid)
        {
            ctx.LogRule("CheckSharedEntityEligibility", "as-err-shared-not-allowed-on-entity", {});
            ctx.EmitAtRange(fact.range, "as-err-shared-not-allowed-on-entity");
        }
    }
}

} // namespace angel_lsp::analysis
