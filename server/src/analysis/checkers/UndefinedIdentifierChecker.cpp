#include "analysis/checkers/UndefinedIdentifierChecker.h"

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

namespace angel_lsp::analysis
{

static MixinRanges CollectMixinRanges(const SymbolTable& symbolTable, const std::string& fileUri)
{
    MixinRanges mixinRanges;
    symbolTable.ForEachSymbolInFile(
        fileUri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
                {
                    mixinRanges.push_back({sym.startLine, sym.endLine});
                }
            }
        });
    return mixinRanges;
}

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
        return ref.isSuperBaseCall || !ctx.request.tree;
    }

    if (ref.name == "function" && ref.isLambdaFunctionKeyword)
    {
        return true;
    }

    if (ref.isCall && ref.name != "super" && ctx.request.tree != nullptr)
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

static bool CheckEnumScopeDiagnostic(const LocalReference& ref, const LocalDefinition* resolved, DiagnosticContext& ctx)
{
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
    return true;
}

static bool IsContainerAccessorProperty(uint32_t byteOffset, std::string_view refName, const DiagnosticContext& ctx)
{
    const auto& index = ctx.request.GetRuleIndex();
    const bool reqKeyword = ctx.request.RequiresAccessorKeyword();
    auto containers = ctx.request.nodeIndex
                          ? ctx.request.nodeIndex->CollectEnclosingContainers(byteOffset, ctx.request.sourceCode)
                          : std::vector<ContainerInfo>{};
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Namespace)
        {
            const std::string nsName = container.qualifiedName.empty() ? container.name : container.qualifiedName;
            const auto& members = index.NamespaceMembers(nsName);
            const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
            if (accSet.contains(refName))
            {
                return true;
            }
            continue;
        }
        if (container.kind != ContainerKind::Class && container.kind != ContainerKind::Interface)
        {
            continue;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(
            container.qualifiedName.empty() ? container.name : container.qualifiedName, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            const auto& members = index.ClassMembers(typeName);
            const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
            if (accSet.contains(refName))
            {
                return true;
            }
        }
    }
    return false;
}

static bool IsEnclosingScopeMember(uint32_t byteOffset, const std::string& refName,
                                   const std::vector<std::string>& usingNamespaces, const DiagnosticContext& ctx)
{
    const auto& index = ctx.request.GetRuleIndex();
    const auto containers = ctx.request.nodeIndex
                                ? ctx.request.nodeIndex->CollectEnclosingContainers(byteOffset, ctx.request.sourceCode)
                                : std::vector<ContainerInfo>{};
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Namespace)
        {
            const std::string nsName = container.qualifiedName.empty() ? container.name : container.qualifiedName;
            if (index.NamespaceMembers(nsName).allMemberNames.contains(refName))
            {
                return true;
            }
            continue;
        }
        if (container.kind != ContainerKind::Class && container.kind != ContainerKind::Interface)
        {
            continue;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(
            container.qualifiedName.empty() ? container.name : container.qualifiedName, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            if (index.ClassMembers(typeName).allMemberNames.contains(refName))
            {
                return true;
            }
        }
    }

    for (const auto& uNs : usingNamespaces)
    {
        if (index.NamespaceMembers(uNs).allMemberNames.contains(refName))
        {
            return true;
        }
    }

    return false;
}

static bool IsAccessorPropertyOrKeyword(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (IsReservedKeyword(ref.name))
    {
        return true;
    }

    const auto& index = ctx.request.GetRuleIndex();
    const auto& globalAccessors = ctx.request.RequiresAccessorKeyword() ? index.keywordGlobalAccessorPropertyNames
                                                                        : index.globalAccessorPropertyNames;
    if (globalAccessors.contains(ref.name))
    {
        return true;
    }

    if (ctx.request.tree)
    {
        if (IsContainerAccessorProperty(ref.startByte, ref.name, ctx))
        {
            return true;
        }
    }

    if (!FindGlobalPropertyAccessors(ref.name, ctx.request.symbolTable, ctx.request.RequiresAccessorKeyword()).empty())
    {
        return true;
    }

    return false;
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
        if (CheckEnumScopeDiagnostic(ref, resolved, uCtx.ctx))
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

        if (uCtx.ctx.request.tree)
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

static void CheckUndefinedIdentifiersIterative(const Scope* rootScope, const UndefinedIdentifierContext& uCtx)
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

void UndefinedIdentifierChecker::Run(const AstCheckContext& ast, DiagnosticContext& ctx)
{
    if (ast.scopeRoot)
    {
        CheckUndefinedIdentifiers(ast.scopeRoot, ctx.request.GetRuleIndex().globalNames, ctx);
    }
}

void UndefinedIdentifierChecker::CheckUndefinedIdentifiers(
    const Scope* scope,
    const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>& knownGlobalNames,
    DiagnosticContext& ctx)
{
    if (!scope)
    {
        return;
    }

    const MixinRanges mixinRanges = CollectMixinRanges(ctx.request.symbolTable, ctx.request.fileUri);
    const std::vector<std::string> usingNamespaces =
        ctx.request.nodeIndex
            ? CollectUsingNamespaces(*ctx.request.nodeIndex, ctx.request.sourceCode)
            : (ctx.request.tree ? CollectUsingNamespaces(ts_tree_root_node(ctx.request.tree), ctx.request.sourceCode)
                                : std::vector<std::string>{});
    const UndefinedIdentifierContext uCtx{knownGlobalNames, mixinRanges, usingNamespaces, ctx};
    CheckUndefinedIdentifiersIterative(scope, uCtx);
}

} // namespace angel_lsp::analysis
