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

static bool ShouldIgnoreReference(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (ref.isMemberAccess || ref.isTypeSpecifier || ref.isNamedArgument)
    {
        return true;
    }

    if (ref.name == "this" || ref.name == "value")
    {
        return true;
    }

    if (ref.name == "super")
    {
        if (ctx.request.tree)
        {
            const TSPoint at{ref.startLine, ref.startCharacter};
            const TSNode node = ts_node_descendant_for_point_range(ts_tree_root_node(ctx.request.tree), at, at);
            if (IsBaseConstructorCall(node, ctx.request.sourceCode))
            {
                return true;
            }
        }
        else
        {
            return true;
        }
    }

    if (ref.name == "function" && ctx.request.tree)
    {
        const TSPoint at{ref.startLine, ref.startCharacter};
        const TSNode node = ts_node_descendant_for_point_range(ts_tree_root_node(ctx.request.tree), at, at);
        const TSNode parent = parser::GetParent(node);
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == parser::GetGrammarSymbols().symLambdaExpression)
        {
            return true;
        }
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

static bool IsEnumReferenceQualified(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (!ctx.request.tree)
    {
        return false;
    }
    const TSPoint at{ref.startLine, ref.startCharacter};
    const TSNode node = ts_node_descendant_for_point_range(ts_tree_root_node(ctx.request.tree), at, at);
    if (ts_node_is_null(node))
    {
        return false;
    }
    const TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    if (ts_node_symbol(parent) == parser::GetGrammarSymbols().symScopedIdentifier)
    {
        const uint32_t pStart = ts_node_start_byte(parent);
        const uint32_t pEnd = ts_node_end_byte(parent);
        if (pStart < ctx.request.sourceCode.size() && pEnd <= ctx.request.sourceCode.size() && pStart < pEnd)
        {
            const std::string_view text = ctx.request.sourceCode.substr(pStart, pEnd - pStart);
            if (utils::ContainsScopeSeparator(text))
            {
                return true;
            }
        }
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
    if (isOwnDeclaration || IsEnumReferenceQualified(ref, ctx))
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

static bool IsContainerAccessorProperty(const TSNode node, std::string_view refName, const DiagnosticContext& ctx)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    const auto& index = ctx.request.GetRuleIndex();
    const bool reqKeyword = ctx.request.RequiresAccessorKeyword();
    auto containers = GetEnclosingContainers(node, ctx.request.sourceCode);
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

static bool IsEnclosingScopeMember(TSNode node, const std::string& refName,
                                   const std::vector<std::string>& usingNamespaces, const DiagnosticContext& ctx)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    const auto& index = ctx.request.GetRuleIndex();
    const auto containers = GetEnclosingContainers(node, ctx.request.sourceCode);
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
        const TSPoint at{ref.startLine, ref.startCharacter};
        const TSNode node = ts_node_descendant_for_point_range(ts_tree_root_node(ctx.request.tree), at, at);
        if (IsContainerAccessorProperty(node, ref.name, ctx))
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

static bool IsReferenceInSyntaxErrorOrMetadata(TSNode root, const LocalReference& ref, const NodeIndex* nodeIndex)
{
    const TSPoint atStart{ref.startLine, ref.startCharacter};
    const TSPoint atEnd{ref.endLine, ref.endCharacter};
    const TSNode node = ts_node_descendant_for_point_range(root, atStart, atEnd);
    if (ts_node_is_null(node))
    {
        return false;
    }
    if (nodeIndex && nodeIndex->NodeHasSyntaxError(node))
    {
        return true;
    }
    bool inErrorOrMeta = false;
    parser::ForEachAncestor(node,
                            [&](TSNode walk) -> bool
                            {
                                if (ts_node_is_error(walk) ||
                                    ts_node_symbol(walk) == parser::GetGrammarSymbols().symMetadata)
                                {
                                    inErrorOrMeta = true;
                                    return false;
                                }
                                return true;
                            });
    return inErrorOrMeta;
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
            const TSNode root = ts_tree_root_node(uCtx.ctx.request.tree);
            if (IsReferenceInSyntaxErrorOrMetadata(root, ref, uCtx.ctx.request.nodeIndex))
            {
                continue;
            }
            const TSPoint at{ref.startLine, ref.startCharacter};
            const TSNode node = ts_node_descendant_for_point_range(root, at, at);
            if (IsEnclosingScopeMember(node, ref.name, uCtx.usingNamespaces, uCtx.ctx))
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
