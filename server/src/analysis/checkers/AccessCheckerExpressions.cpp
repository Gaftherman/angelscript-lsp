#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/MemberAccessFacts.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/** @brief True when derived is base, or reaches it through its declared base chain. */
static bool DerivesFrom(std::string_view derived, std::string_view base, const SymbolTable& table)
{
    return IsDerivedFrom(derived, base, table);
}

/**
 * @brief Resolves the owning type of a member access object expression.
 *
 * @param[in] objectBytes Byte span of the object expression.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context.
 * @return Normalized owner type name.
 */
static std::string ResolveObjectOwnerType(IndexedByteRange objectBytes, const Scope* scope,
                                          const DiagnosticContext& ctx)
{
    const std::string_view arrayContainer = ctx.request.GetEffectiveArrayTypeName();
    return MemberOwnerType(ResolveExpressionType(objectBytes, ExpressionTypeContext(scope, ctx)), arrayContainer);
}

/**
 * @brief Emits a hint diagnostic if access was made via a disabled accessor.
 *
 * @param[in] fact Member expression fact.
 * @param[in] member Member resolution outcome.
 * @param[in] objectType Object type name.
 * @param[in,out] ctx Diagnostic context.
 */
static void MaybeEmitAccessorHint(const MemberExpressionFact& fact, const MemberLookupResult& member,
                                  std::string_view objectType, DiagnosticContext& ctx)
{
    if (member.viaAccessor && !ctx.request.ScriptAccessorsAreProperties() && ctx.request.diagnostics &&
        ctx.request.diagnostics->reportAccessorDisabled)
    {
        ctx.EmitAtRange(fact.memberRange, "as-hint-accessor-disabled", {objectType, fact.memberName},
                        DiagnosticSeverity::Hint);
    }
}

/**
 * @brief Determines whether a member access is permitted based on privacy/protection rules.
 *
 * @param[in] member Resolved member access descriptor.
 * @param[in] accessingClass Enclosing class making the access.
 * @param[in] objectType Type of the target object expression.
 * @param[in] ctx Diagnostic context.
 * @return True if member access is allowed, false if access violation.
 */
static bool IsMemberAccessAllowed(const MemberLookupResult& member, std::string_view accessingClass,
                                  std::string_view objectType, const DiagnosticContext& ctx)
{
    const bool declaredPrivate = member.access == AccessModifier::Private;
    if (declaredPrivate && !ctx.request.TreatsPrivateAsProtected())
    {
        return IsSameAccessType(accessingClass, member.declaringClass);
    }
    return !accessingClass.empty() && DerivesFrom(accessingClass, member.declaringClass, ctx.request.symbolTable) &&
           DerivesFrom(objectType, accessingClass, ctx.request.symbolTable);
}

/**
 * @brief Emits a private or protected access violation diagnostic for a member access.
 *
 * @param[in] memberRange Source range of the member being accessed.
 * @param[in] member Member resolution outcome.
 * @param[in] memberName Name of the member being accessed.
 * @param[in,out] ctx Diagnostic context.
 */
static void EmitPrivacyViolation(SourceRange memberRange, const MemberLookupResult& member, std::string_view memberName,
                                 DiagnosticContext& ctx)
{
    const bool declaredPrivate = member.access == AccessModifier::Private;
    ctx.EmitAtRange(memberRange,
                    declaredPrivate ? diagnostics::codes::PrivateMemberAccess
                                    : diagnostics::codes::ProtectedMemberAccess,
                    memberName, member.declaringClass);
}

/**
 * @brief Validates accessibility permissions for an accessed member symbol.
 *
 * @param[in] fact Member expression fact.
 * @param[in] member Member resolution outcome.
 * @param[in] objectType Object type name.
 * @param[in,out] ctx Diagnostic context.
 */
static void ValidateMemberPrivacy(const MemberExpressionFact& fact, const MemberLookupResult& member,
                                  std::string_view objectType, DiagnosticContext& ctx)
{
    if (!member.decided)
    {
        return;
    }
    const std::string_view accessingClass = fact.enclosingClassName;
    if (fact.insideMixin && (fact.isThisAccess || IsSameAccessType(accessingClass, member.declaringClass)))
    {
        return;
    }
    if (!IsMemberAccessAllowed(member, accessingClass, objectType, ctx))
    {
        EmitPrivacyViolation(fact.memberRange, member, fact.memberName, ctx);
    }
}

void CheckMemberExpression(const MemberExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const std::string objectType = ResolveObjectOwnerType(fact.objectBytes, scope, ctx);
    if (objectType.empty() || objectType == "auto")
    {
        return;
    }

    if (fact.memberName.empty())
    {
        return;
    }

    if (IsReservedKeyword(fact.memberName))
    {
        ctx.EmitAtRange(fact.memberRange, diagnostics::codes::ReservedKeywordName, fact.memberName);
        return;
    }

    if ((IsCorePrimitive(objectType) && objectType != "auto") || objectType == "void")
    {
        ctx.EmitAtRange(fact.memberRange, diagnostics::codes::MemberNotFound, objectType, fact.memberName);
        return;
    }

    if (!HierarchyIsFullyVisible(objectType, table))
    {
        return;
    }

    const MemberLookupResult member = FindMemberWithProxies(objectType, fact.memberName, table, ctx);
    if (!member.found)
    {
        ctx.EmitAtRange(fact.memberRange, diagnostics::codes::MemberNotFound, objectType, fact.memberName);
        return;
    }

    MaybeEmitAccessorHint(fact, member, objectType, ctx);
    ValidateMemberPrivacy(fact, member, objectType, ctx);
}

} // namespace angel_lsp::analysis
