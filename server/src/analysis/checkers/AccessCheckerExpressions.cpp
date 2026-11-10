#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/** @brief True when derived is base, or reaches it through its declared base chain. */
static bool DerivesFrom(std::string_view derived, std::string_view base, const SymbolTable& table)
{
    return IsDerivedFrom(derived, base, table);
}

/** @brief Innermost class body enclosing a node, or empty when the node sits outside one. */
std::string_view EnclosingClass(TSNode node, std::string_view sourceCode, bool& insideMixin, const NodeIndex* nodeIndex)
{
    insideMixin = false;
    if (ts_node_is_null(node) || !nodeIndex)
    {
        return {};
    }

    const uint32_t byteOffset = ts_node_start_byte(node);
    const auto* span = nodeIndex->FindInnermostClassSpan(byteOffset);
    if (span)
    {
        insideMixin = span->isMixin;
        if (span->nameStartByte < span->nameEndByte && span->nameEndByte <= sourceCode.size())
        {
            return sourceCode.substr(span->nameStartByte, span->nameEndByte - span->nameStartByte);
        }
    }
    return {};
}

/**
 * @brief Resolves the owning type of a member access object expression.
 *
 * @param[in] objectNode AST node of the object expression.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context.
 * @return Normalized owner type name.
 */
static std::string ResolveObjectOwnerType(TSNode objectNode, const Scope* scope, const DiagnosticContext& ctx)
{
    const std::string_view arrayContainer = ctx.request.GetEffectiveArrayTypeName();
    return MemberOwnerType(ResolveExpressionType(objectNode, ExpressionTypeContext(scope, ctx)), arrayContainer);
}

/**
 * @brief Emits a hint diagnostic if access was made via a disabled accessor.
 *
 * @param[in] memberNode AST node of the member being accessed.
 * @param[in] member Member resolution outcome.
 * @param[in] objectType Object type name.
 * @param[in,out] ctx Diagnostic context.
 */
static void MaybeEmitAccessorHint(TSNode memberNode, const MemberLookupResult& member, const std::string& objectType,
                                  DiagnosticContext& ctx)
{
    if (member.viaAccessor && !ctx.request.ScriptAccessorsAreProperties() && ctx.request.diagnostics &&
        ctx.request.diagnostics->reportAccessorDisabled)
    {
        const TSPoint hintStart = ts_node_start_point(memberNode);
        const TSPoint hintEnd = ts_node_end_point(memberNode);
        ctx.EmitAtRange({hintStart.row, hintStart.column, hintEnd.row, hintEnd.column}, "as-hint-accessor-disabled",
                        {objectType, std::string(parser::NodeText(memberNode, ctx.request.sourceCode))},
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
 * @param[in] memberNode AST node of the member being accessed.
 * @param[in] member Member resolution outcome.
 * @param[in] memberName Name of the member being accessed.
 * @param[in,out] ctx Diagnostic context.
 */
static void EmitPrivacyViolation(TSNode memberNode, const MemberLookupResult& member, std::string_view memberName,
                                 DiagnosticContext& ctx)
{
    const TSPoint start = ts_node_start_point(memberNode);
    const TSPoint end = ts_node_end_point(memberNode);
    const bool declaredPrivate = member.access == AccessModifier::Private;
    ctx.EmitAtRange({start.row, start.column, end.row, end.column},
                    declaredPrivate ? diagnostics::codes::PrivateMemberAccess
                                    : diagnostics::codes::ProtectedMemberAccess,
                    memberName, member.declaringClass);
}

static void ValidateMemberPrivacy(TSNode node, const MemberLookupResult& member, const std::string& objectType,
                                  DiagnosticContext& ctx)
{
    if (!member.decided)
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode objectNode = parser::GetChildByField(node, gs.fieldObject);
    TSNode memberNode = parser::GetChildByField(node, gs.fieldMember);
    bool insideMixin = false;
    const std::string_view accessingClass =
        EnclosingClass(node, ctx.request.sourceCode, insideMixin, ctx.request.nodeIndex);
    const bool isThisAccess = parser::NodeText(objectNode, ctx.request.sourceCode) == "this";
    if (insideMixin && (isThisAccess || IsSameAccessType(accessingClass, member.declaringClass)))
    {
        return;
    }
    if (!IsMemberAccessAllowed(member, accessingClass, objectType, ctx))
    {
        const std::string memberName(parser::NodeText(memberNode, ctx.request.sourceCode));
        EmitPrivacyViolation(memberNode, member, memberName, ctx);
    }
}

void CheckMemberExpression(TSNode node, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode objectNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldObject);
    TSNode memberNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string objectType = ResolveObjectOwnerType(objectNode, scope, ctx);
    if (objectType.empty() || objectType == "auto")
    {
        return;
    }

    const std::string memberName(parser::NodeText(memberNode, request.sourceCode));
    if (memberName.empty())
    {
        return;
    }

    if (parser::keywords::IsReserved(memberName))
    {
        const TSPoint start = ts_node_start_point(memberNode);
        const TSPoint end = ts_node_end_point(memberNode);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::ReservedKeywordName,
                        memberName);
        return;
    }

    if ((IsCorePrimitive(objectType) && objectType != "auto") || objectType == "void")
    {
        const TSPoint start = ts_node_start_point(memberNode);
        const TSPoint end = ts_node_end_point(memberNode);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::MemberNotFound, objectType,
                        memberName);
        return;
    }

    if (!HierarchyIsFullyVisible(objectType, table))
    {
        return;
    }

    const MemberLookupResult member = FindMemberWithProxies(objectType, memberName, table, ctx);
    if (!member.found)
    {
        const TSPoint start = ts_node_start_point(memberNode);
        const TSPoint end = ts_node_end_point(memberNode);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::MemberNotFound, objectType,
                        memberName);
        return;
    }

    MaybeEmitAccessorHint(memberNode, member, objectType, ctx);
    ValidateMemberPrivacy(node, member, objectType, ctx);
}

} // namespace angel_lsp::analysis
