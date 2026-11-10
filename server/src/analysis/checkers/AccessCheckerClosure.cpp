#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/MemberAccessFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a name is declared within the closure's local scope chain.
 *
 * @param[in] scope Current inner scope.
 * @param[in] enclosingClosure Enclosing closure scope.
 * @param[in] idText Name to check.
 * @return True if declared within closure.
 */
static bool IsDeclaredInClosure(const Scope* scope, const Scope* enclosingClosure, std::string_view idText)
{
    for (const Scope* s = scope; s != nullptr; s = s->parent)
    {
        for (const auto& def : s->definitions)
        {
            if (def.name == idText)
            {
                return true;
            }
        }
        if (s == enclosingClosure)
        {
            break;
        }
    }
    return false;
}

/**
 * @brief Checks if a name is declared as a local variable or parameter in outer non-class scopes.
 *
 * @param[in] enclosingClosure Enclosing closure scope.
 * @param[in] idText Name to check.
 * @return True if declared in an outer function scope.
 */
static bool IsOuterLocalOrParameter(const Scope* enclosingClosure, std::string_view idText)
{
    for (const Scope* s = enclosingClosure->parent; s != nullptr; s = s->parent)
    {
        if (s->kind == ScopeKind::Class || s->kind == ScopeKind::Namespace || s->kind == ScopeKind::Global)
        {
            break;
        }
        for (const auto& def : s->definitions)
        {
            if (def.name == idText &&
                (def.kind == LocalDefinitionKind::Variable || def.kind == LocalDefinitionKind::Parameter))
            {
                return true;
            }
        }
    }
    return false;
}

bool CheckClosureDisallowedAccess(const MemberAccessIdentifierFact& fact, const Scope* scope, DiagnosticContext& ctx)
{
    const Scope* enclosingClosure = FindEnclosingClosure(scope);
    if (!enclosingClosure)
    {
        return false;
    }

    if (fact.isThisOrSuper)
    {
        ctx.EmitAtRange(fact.range, diagnostics::codes::LambdaThisDisallowed);
        return true;
    }

    if (IsDeclaredInClosure(scope, enclosingClosure, fact.name))
    {
        return false;
    }

    if (IsOuterLocalOrParameter(enclosingClosure, fact.name))
    {
        ctx.EmitAtRange(fact.range, diagnostics::codes::LambdaClosureDisallowed, fact.name);
        return true;
    }

    const std::string_view outerClass = fact.enclosingClassOfLambda;
    if (!outerClass.empty())
    {
        const MemberLookupResult member = FindMember(outerClass, fact.name, ctx.request.symbolTable, false);
        if (member.found)
        {
            ctx.EmitAtRange(fact.range, diagnostics::codes::LambdaMemberAccess, fact.name);
            return true;
        }
    }

    return false;
}

} // namespace angel_lsp::analysis
