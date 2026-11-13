#include "analysis/checkers/UndefinedIdentifierCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
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

static bool ShouldIgnoreReference(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (ref.isMemberAccess || ref.isTypeSpecifier || ref.isNamedArgument || IsScopedIdentifierPrefix(ref))
    {
        return true;
    }

    if (ref.name == "this" || ref.name == "value")
    {
        return true;
    }

    if (ref.name == "super")
    {
        return ref.isSuperBaseCall || !ctx.request.nodeIndex;
    }

    if (ref.name == "function" && ref.isLambdaFunctionKeyword)
    {
        return true;
    }

    if (ref.isCall && ref.name != "super" && ctx.request.nodeIndex != nullptr)
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

static void CheckScopeReferences(const Scope* scope, const UndefinedIdentifierContext& uCtx)
{
    for (const auto& ref : scope->references)
    {
        if (ShouldIgnoreReference(ref, uCtx.ctx) || IsReferenceInMixin(ref, uCtx.mixinRanges))
        {
            continue;
        }

        const LocalDefinition* resolved = ResolveInScopeAt(scope, ref.name, ref.startByte);
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
