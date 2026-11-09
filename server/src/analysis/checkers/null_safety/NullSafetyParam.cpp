#include "analysis/checkers/null_safety/NullSafetyParam.h"

#include "analysis/checkers/null_safety/NullSafetyTypes.h"
#include "analysis/index/NullFlowFacts.h"
#include "analysis/index/ScopeTree.h"

#include <cstdint>
#include <string_view>

namespace angel_lsp::analysis
{
static void CollectParametersFromScope(const NullFlowCallable& callable, const Scope* scopeRoot, FlowState& state)
{
    if (!scopeRoot || callable.body == NoNullFlowNode)
    {
        return;
    }
    const Scope* scope = FindInnermostScope(scopeRoot, callable.bodyLine, callable.bodyColumn);
    ScopeTraversalGuard guard;
    while (scope && !scope->isFunctionScope && scope->parent && guard.CheckAndInsert(scope))
    {
        scope = scope->parent;
    }
    if (scope)
    {
        for (const auto& def : scope->definitions)
        {
            if (def.kind == LocalDefinitionKind::Parameter && def.isHandleType)
            {
                state.vars[std::string_view(def.name)] = Nullability::Nullable;
            }
        }
    }
}

/**
 * @brief Collects handle parameters from scope tree or AST parameter lists into initial flow state.
 * @param[in] callable Extracted function or lambda with its body position and handle parameters.
 * @param[in] ctx Lexical scopes and null flow facts for the revision.
 * @param[in,out] state Flow state to populate with nullable parameters.
 */
void CollectParameters(const NullFlowCallable& callable, const NullCheckContext& ctx, FlowState& state)
{
    CollectParametersFromScope(callable, ctx.scopeRoot, state);
    const uint32_t end = callable.parameterBegin + callable.parameterCount;
    for (uint32_t i = callable.parameterBegin; i < end && i < ctx.facts.parameters.size(); ++i)
    {
        state.vars[std::string_view(ctx.facts.parameters[i].name)] = Nullability::Nullable;
    }
}

} // namespace angel_lsp::analysis
