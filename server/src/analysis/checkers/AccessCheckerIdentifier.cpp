#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/index/MemberAccessFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if the lexical scope chain includes a class scope.
 *
 * @param[in] scope Lexical scope to inspect.
 * @return True if scope is null (unknown) or has an enclosing ScopeKind::Class.
 */
static bool HasEnclosingClassScope(const Scope* scope)
{
    if (!scope)
    {
        return true;
    }
    for (const Scope* s = scope; s != nullptr; s = s->parent)
    {
        if (s->kind == ScopeKind::Class)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Validates implicit member access on an identifier within an enclosing class.
 *
 * @param[in] fact Pre-extracted member access identifier fact.
 * @param[in] scope Current lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckImplicitMemberAccess(const MemberAccessIdentifierFact& fact, const Scope* scope,
                                      DiagnosticContext& ctx)
{
    if (!HasEnclosingClassScope(scope))
    {
        return;
    }

    const LocalDefinition* localDef = ResolveInScopeAt(scope, fact.name, fact.bytes.startByte);
    if (localDef &&
        (localDef->kind == LocalDefinitionKind::Variable || localDef->kind == LocalDefinitionKind::Parameter))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string_view accessingClass = fact.enclosingClassName;
    if (accessingClass.empty())
    {
        return;
    }

    const MemberLookupResult member =
        FindMember(accessingClass, fact.name, table, ctx.request.RequiresAccessorKeyword());
    if (!member.decided)
    {
        return;
    }

    const bool declaredPrivate = member.access == AccessModifier::Private;
    if (declaredPrivate && !ctx.request.TreatsPrivateAsProtected())
    {
        if (fact.insideMixin &&
            (IsSameAccessType(accessingClass, member.declaringClass) || member.declaringClass == accessingClass))
        {
            return;
        }
        if (!IsSameAccessType(accessingClass, member.declaringClass))
        {
            ctx.EmitAtRange(fact.range, "as-err-private-member-access", fact.name, member.declaringClass);
        }
    }
}

void CheckIdentifierFact(const MemberAccessIdentifierFact& fact, const Scope* scope, DiagnosticContext& ctx)
{
    if (fact.isThisOrSuper)
    {
        CheckClosureDisallowedAccess(fact, scope, ctx);
        return;
    }

    if (CheckClosureDisallowedAccess(fact, scope, ctx))
    {
        return;
    }

    CheckImplicitMemberAccess(fact, scope, ctx);
}

} // namespace angel_lsp::analysis
