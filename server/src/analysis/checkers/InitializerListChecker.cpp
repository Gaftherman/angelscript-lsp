#include "analysis/checkers/InitializerListChecker.h"

#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/resolution/OperandRangeResolution.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Computes the element context at the position of an initializer list fact.
 * @param[in] fact       Initializer list fact.
 * @param[in] sourceCode Document source text.
 * @param[in] scopeRoot  Root of the lexical scope tree, or nullptr.
 * @return ElementContext with enclosing scope.
 */
static ElementContext ElementsAt(const InitializerListFact& fact, std::string_view sourceCode, const Scope* scopeRoot)
{
    return ElementContext{sourceCode,
                          scopeRoot ? FindEnclosingScope(scopeRoot, fact.range.startLine, fact.range.startCharacter)
                                    : nullptr};
}

/**
 * @brief Validates typed initializer lists (`Type = { ... }`).
 * @param[in] fact   Initializer list fact.
 * @param[in] valCtx Active list validation context.
 */
static void ProcessTypedInitializerList(const InitializerListFact& fact, const ListValidationContext& valCtx)
{
    if (!fact.owner.targetType.empty())
    {
        InitializerListValidator::ValidateList(fact, fact.owner.targetType, valCtx);
    }
}

/**
 * @brief Validates assignment expressions with initializer list right-hand sides (`a = { ... }`).
 * @param[in] fact   Initializer list fact.
 * @param[in] valCtx Active list validation context.
 */
static void ProcessAssignmentExpression(const InitializerListFact& fact, const ListValidationContext& valCtx)
{
    if (fact.owner.targetBytes.Empty())
    {
        return;
    }
    const std::string targetType =
        ResolveExpressionType(fact.owner.targetBytes, ExpressionTypeContext(valCtx.elements.scopeRoot, valCtx.ctx));
    if (!targetType.empty())
    {
        InitializerListValidator::ValidateList(fact, targetType, valCtx);
    }
}

/**
 * @brief Validates return statements returning initializer lists (`return { ... };`).
 * @param[in] fact   Initializer list fact.
 * @param[in] valCtx Active list validation context.
 */
static void ProcessReturnStatement(const InitializerListFact& fact, const ListValidationContext& valCtx)
{
    if (!fact.owner.targetType.empty())
    {
        InitializerListValidator::ValidateList(fact, fact.owner.targetType, valCtx);
    }
}

/**
 * @brief Validates variable declarations with initializer list initializers (`Type a = { ... };`).
 * @param[in] fact   Initializer list fact.
 * @param[in] valCtx Active list validation context.
 */
static void ProcessVariableDeclaration(const InitializerListFact& fact, const ListValidationContext& valCtx)
{
    if (!fact.owner.targetType.empty())
    {
        InitializerListValidator::ValidateList(fact, fact.owner.targetType, valCtx);
    }
}

/**
 * @brief Dispatches a top-level initializer list fact to its owning context processor.
 * @param[in] fact   Initializer list fact.
 * @param[in] valCtx Active list validation context.
 */
static void DispatchInitListFact(const InitializerListFact& fact, const ListValidationContext& valCtx)
{
    switch (fact.owner.kind)
    {
    case InitializerListOwnerKind::TypedList:
        ProcessTypedInitializerList(fact, valCtx);
        break;
    case InitializerListOwnerKind::Assignment:
        ProcessAssignmentExpression(fact, valCtx);
        break;
    case InitializerListOwnerKind::ReturnStatement:
        ProcessReturnStatement(fact, valCtx);
        break;
    case InitializerListOwnerKind::VariableDeclaration:
        ProcessVariableDeclaration(fact, valCtx);
        break;
    case InitializerListOwnerKind::None:
    case InitializerListOwnerKind::CallArgument:
    case InitializerListOwnerKind::ParentList:
        break;
    }
}

void CheckInitializerListAgainstType(const InitializerListTargetRequest& request, DiagnosticContext& ctx)
{
    StructLayoutCache cache;
    const ListValidationContext valCtx{ctx, request.elements, cache, request.facts, 0};
    InitializerListValidator::ValidateList(request.list, request.targetType, valCtx);
}

void CheckInitializerListAgainstType(const InitializerListFact& list, const InitializerListFacts& facts,
                                     std::string_view targetType, DiagnosticContext& ctx)
{
    const ElementContext elements{ctx.request.sourceCode, ctx.request.scopeRoot.get()};
    CheckInitializerListAgainstType(InitializerListTargetRequest{list, facts, targetType, elements}, ctx);
}

void ValidateInitializerList(const InitializerListFact& list, const InitializerListFacts& facts,
                             std::string_view expectedType, DiagnosticContext& ctx)
{
    const ElementContext elements{ctx.request.sourceCode, ctx.request.scopeRoot.get()};
    StructLayoutCache cache;
    const ListValidationContext valCtx{ctx, elements, cache, facts, 0};
    InitializerListValidator::ValidateList(list, expectedType, valCtx);
}

void CheckInitializerLists(const InitializerListFacts& facts, std::string_view sourceCode, const Scope* scopeRoot,
                           DiagnosticContext& ctx)
{
    StructLayoutCache structCache;
    for (const auto& fact : facts.lists)
    {
        if (fact.parentListIndex != NoInitializerList)
        {
            continue;
        }
        const ElementContext elements = ElementsAt(fact, sourceCode, scopeRoot);
        const ListValidationContext valCtx{ctx, elements, structCache, facts, 0};
        DispatchInitListFact(fact, valCtx);
    }
}

void CheckInitializerLists(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return;
    }
    const InitializerListFacts facts = BuildInitializerListFacts(*request.nodeIndex, request.sourceCode);
    CheckInitializerLists(facts, request.sourceCode, request.scopeRoot, ctx);
}

} // namespace angel_lsp::analysis
