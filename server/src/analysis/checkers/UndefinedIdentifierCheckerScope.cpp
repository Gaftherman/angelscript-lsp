#include "analysis/checkers/UndefinedIdentifierCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "parser/Keywords.h"
#include "utils/ScopedNameUtils.h"

#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsScopedIdentifierPrefix(const LocalReference& ref) noexcept
{
    return ref.isScopedIdentifier && ref.startByte == ref.receiverStartByte && ref.endByte == ref.receiverEndByte &&
           ref.endByte < ref.qualifierEndByte;
}

/**
 * @brief Checks whether a reference has no explicit scope qualifier.
 * @param[in] ref Indexed reference whose enclosing scoped identifier may wrap a single name.
 * @return True when the reference has no scoped identifier wrapper or fills its entire span.
 */
static bool IsUnqualifiedReference(const LocalReference& ref) noexcept
{
    return !ref.isScopedIdentifier || (ref.startByte == ref.qualifierStartByte && ref.endByte == ref.qualifierEndByte);
}

static bool ShouldIgnoreReference(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (ref.isMemberAccess || ref.isTypeSpecifier || ref.isNamedArgument || IsScopedIdentifierPrefix(ref))
    {
        return true;
    }

    if (ref.name == parser::keywords::This)
    {
        return true;
    }

    if (ref.name == parser::keywords::Super)
    {
        return ref.isSuperBaseCall || !ctx.request.nodeIndex;
    }

    if (ref.name == parser::keywords::Function && ref.isLambdaFunctionKeyword)
    {
        return true;
    }

    if (ref.isCall && ref.name != parser::keywords::Super && ctx.request.nodeIndex != nullptr)
    {
        return true;
    }

    return false;
}

static bool IsReferenceInMixin(const LocalReference& ref, const MixinRanges& mixinRanges)
{
    for (const auto& r : mixinRanges)
    {
        if (ref.startLine >= r.first && ref.endLine <= r.second)
        {
            return true;
        }
    }
    return false;
}

static bool IsEnumReferenceQualified(const LocalReference& ref, std::string_view sourceCode)
{
    if (ref.isScopedIdentifier && ref.qualifierStartByte < ref.qualifierEndByte &&
        ref.qualifierEndByte <= sourceCode.size())
    {
        const std::string_view scoped =
            sourceCode.substr(ref.qualifierStartByte, ref.qualifierEndByte - ref.qualifierStartByte);
        return utils::ContainsScopeSeparator(scoped);
    }
    return false;
}

static bool CheckEnumScopeDiagnostic(const LocalReference& ref, const LocalDefinition* resolved,
                                     const UndefinedIdentifierContext& uCtx)
{
    DiagnosticContext& ctx = uCtx.ctx;
    if (resolved != nullptr && resolved->kind != LocalDefinitionKind::Constant)
    {
        return false;
    }
    if (!ctx.request.GetRuleIndex().enumMemberNames.contains(ref.name))
    {
        return false;
    }

    const bool isOwnDeclaration =
        resolved != nullptr && resolved->startLine == ref.startLine && resolved->startCharacter == ref.startCharacter;
    if (isOwnDeclaration || IsEnumReferenceQualified(ref, ctx.request.sourceCode))
    {
        return false;
    }

    if (ctx.request.RequiresEnumScope())
    {
        ctx.EmitAtRange({ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter},
                        "as-err-enum-scope-required", ref.name);
        return true;
    }
    // Without the scope requirement a value is an ordinary name of its own namespace: it is reachable
    // from that namespace, its children and importers, but not from a sibling or an unrelated one.
    return IsEnumValueReachable(ref.startByte, ref.name, uCtx.usingNamespaces, ctx);
}

static bool IsReferenceInSyntaxErrorOrMetadata(const LocalReference& ref, const NodeIndex* nodeIndex)
{
    if (ref.inMetadataOrError)
    {
        return true;
    }
    if (nodeIndex && nodeIndex->IsInsideError(ref.startByte))
    {
        return true;
    }
    return false;
}

/**
 * @brief Finds the method receiver available to unqualified references in a scope.
 * @param[in] scope Scope whose references will be checked.
 * @param[in] ctx Active diagnostics settings.
 * @return Qualified class name, or an empty view when the warning or receiver is unavailable.
 */
static std::string_view ImplicitReceiver(const Scope& scope, const DiagnosticContext& ctx)
{
    const auto* settings = ctx.request.diagnostics;
    if (!settings || !settings->reportMissingClassAccessor || scope.GetEnclosingClass().empty() ||
        scope.GetCaller().empty())
    {
        return {};
    }
    for (const Scope* enclosing = &scope; enclosing; enclosing = enclosing->parent)
    {
        if (enclosing->kind == ScopeKind::Closure)
        {
            return {};
        }
    }
    return scope.GetEnclosingClass();
}

static void CheckScopeReferences(const Scope* scope, const UndefinedIdentifierContext& uCtx)
{
    const auto receiver = ImplicitReceiver(*scope, uCtx.ctx);
    for (const auto& ref : scope->references)
    {
        if (ShouldIgnoreReference(ref, uCtx.ctx) || IsReferenceInMixin(ref, uCtx.mixinRanges))
        {
            continue;
        }

        const Scope* declaration = nullptr;
        const LocalDefinition* resolved =
            ResolveInScopeAt(scope, ref.name, ref.startByte, receiver.empty() ? nullptr : &declaration);
        if (!receiver.empty() && IsUnqualifiedReference(ref) &&
            !IsReferenceInSyntaxErrorOrMetadata(ref, uCtx.ctx.request.nodeIndex))
        {
            const bool ownField = resolved && declaration && declaration->kind == ScopeKind::Class &&
                                  resolved->kind == LocalDefinitionKind::Field;
            const bool outsideClass =
                declaration && (declaration->kind == ScopeKind::Global || declaration->kind == ScopeKind::Namespace);
            if (ownField || ((!resolved || outsideClass) && IsAccessibleInstanceProperty(receiver, ref.name, uCtx.ctx)))
            {
                uCtx.ctx.EmitAtRange({ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter},
                                     diagnostics::codes::ImplicitMemberAccess, {ref.name, ref.name},
                                     DiagnosticSeverity::Warning);
                continue;
            }
        }
        if (CheckEnumScopeDiagnostic(ref, resolved, uCtx))
        {
            continue;
        }

        if (resolved != nullptr || ResolveInScopeAt(scope, ref.name, ref.startByte, nullptr, false) != nullptr ||
            uCtx.knownGlobalNames.contains(ref.name))
        {
            continue;
        }

        if (IsAccessorPropertyOrKeyword(ref, uCtx.ctx))
        {
            continue;
        }

        if (uCtx.ctx.request.nodeIndex != nullptr)
        {
            if (IsReferenceInSyntaxErrorOrMetadata(ref, uCtx.ctx.request.nodeIndex))
            {
                continue;
            }
            if (IsEnclosingScopeMember(ref.startByte, ref.name, uCtx.usingNamespaces, uCtx.ctx))
            {
                continue;
            }
        }

        uCtx.ctx.EmitAtRange({ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter},
                             "as-warn-undeclared-identifier", ref.name, DiagnosticSeverity::Error);
    }
}

void CheckUndefinedIdentifiersIterative(const Scope* rootScope, const UndefinedIdentifierContext& uCtx)
{
    if (!rootScope)
    {
        return;
    }

    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        CheckScopeReferences(scope, uCtx);

        for (const auto& child : scope->children)
        {
            if (child)
            {
                worklist.push_back(child.get());
            }
        }
    }
}

} // namespace angel_lsp::analysis
