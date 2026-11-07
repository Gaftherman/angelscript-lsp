#include "analysis/resolution/ReferenceCounting.h"

#include "analysis/resolution/ReferenceCountingAccess.h"
#include "analysis/resolution/ReferenceCountingScope.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "i18n/i18n.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
static void CountStringLiteralReferences(std::string_view fileUri, const Scope& scope, const BatchReferenceContext& ctx)
{
    if (scope.stringLiteralCallRefs)
    {
        for (const auto& strRef : *scope.stringLiteralCallRefs)
        {
            auto it = ctx.targetsByName.find(strRef.name);
            if (it != ctx.targetsByName.end())
            {
                const uint64_t pos = (static_cast<uint64_t>(strRef.line) << 32) | strRef.column;
                for (size_t targetIdx : it->second)
                {
                    ctx.targets[targetIdx].seenRefs.insert({std::string(fileUri), pos});
                }
            }
        }
    }
}

static void ProcessScopeReferencesBatch(std::string_view fileUri, const Scope* scope, const BatchReferenceContext& ctx,
                                        const CallBindings* bindings)
{
    for (const auto& ref : scope->references)
    {
        auto it = ctx.targetsByName.find(ref.name);
        if (it == ctx.targetsByName.end())
        {
            continue;
        }

        const auto* bound = ref.isCall && bindings ? bindings->Find(ref.startByte) : nullptr;
        const uint64_t pos = (static_cast<uint64_t>(ref.startLine) << 32) | ref.startCharacter;
        for (size_t targetIdx : it->second)
        {
            auto& target = ctx.targets[targetIdx];
            if (target.seenRefs.contains({std::string(fileUri), pos}))
            {
                continue;
            }
            if (bound && target.isFunction &&
                (target.compatibleClasses.empty() || target.group.front().qualifiedName == bound->qualifiedName))
            {
                continue;
            }
            ReferenceCollectionCriteria criteria{
                .targetName = target.targetName,
                .group = target.group,
                .compatibleClasses = target.compatibleClasses,
                .allDeclRanges = target.allDeclRanges,
                .targetNamespace = target.targetNamespace,
                .targetEnumContainer = target.targetEnumContainer,
                .isEnumConstant = target.isEnumConstant,
                .targetAccess = target.targetAccess,
                .isFunction = target.isFunction,
                .minArgs = target.minArgs,
                .maxArgs = target.maxArgs,
                .symbolTable = ctx.request.symbolTable,
                .request = ctx.request,
                .classCache = ctx.classCache,
            };
            if (!IsDeclarationOrDefinition(fileUri, ref, scope, criteria) && MatchesCallArguments(ref, criteria) &&
                IsValidAccess(fileUri, ref, scope, criteria))
            {
                target.seenRefs.insert({std::string(fileUri), pos});
            }
        }
    }
    CountStringLiteralReferences(fileUri, *scope, ctx);
}

static void CollectReferencesInScopeTreeBatch(std::string_view fileUri, const Scope* root,
                                              const BatchReferenceContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const Scope*> stack{root};
    while (!stack.empty())
    {
        const Scope* scope = stack.back();
        stack.pop_back();
        ProcessScopeReferencesBatch(fileUri, scope, ctx, root->GetCallBindings());
        for (const auto& child : scope->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

static void CountBoundCalls(ReferenceCountTarget& target, const ReferenceCountRequest& request)
{
    if (!target.isFunction)
    {
        return;
    }
    static const ModuleFilter kEmptyFilter = std::nullopt;
    const auto& moduleFilter = request.occurrenceModule ? *request.occurrenceModule : kEmptyFilter;
    for (const auto& symbol : target.group)
    {
        if (symbol.type != SymbolType::Function || symbol.isSynthesized)
        {
            continue;
        }
        request.scopeIndex.ForEachBoundUse(
            CallDeclaration(symbol), moduleFilter,
            [&](const std::string& uri, std::span<const SourceRange> ranges)
            {
                for (const auto& range : ranges)
                {
                    const uint64_t position = (static_cast<uint64_t>(range.startLine) << 32) | range.startCharacter;
                    target.seenRefs.insert({uri, position});
                }
            });
    }
}

void CountReferences(std::span<ReferenceCountTarget> targets, const TargetIndexMap& targetsByName,
                     const ReferenceCountRequest& request)
{
    for (auto& target : targets)
    {
        CountBoundCalls(target, request);
    }
    BatchReferenceContext ctx{targets, targetsByName, request};
    static const ModuleFilter kEmptyFilter = std::nullopt;
    const auto& moduleFilter = request.occurrenceModule ? *request.occurrenceModule : kEmptyFilter;
    request.scopeIndex.ForEachOccurrenceScope(
        moduleFilter,
        [&](const std::string& fileUri, const std::shared_ptr<const Scope>& root)
        {
            if (root && !angel_lsp::utils::IsPredefinedFile(fileUri, request.predefinedExtension))
            {
                CollectReferencesInScopeTreeBatch(fileUri, root.get(), ctx);
            }
        });
}

std::string FormatReferenceCount(size_t refCount, const i18n::I18n* i18n)
{
    return (refCount == 1) ? i18n::FormatMessage(i18n, "codelens.reference", "1 reference")
                           : i18n::FormatMessage(i18n, "codelens.references", "{} references", refCount);
}

void AddSymbolDeclRanges(const std::vector<Symbol>& syms, DeclRangeSet& declRanges)
{
    for (const auto& s : syms)
    {
        if (s.type != SymbolType::CallReference)
        {
            uint32_t sL = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0)
                              ? s.selectionRange.startLine
                              : s.startLine;
            uint32_t sC = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0)
                              ? s.selectionRange.startCharacter
                              : s.startCharacter;
            declRanges.insert({s.fileUri, sL, sC});
        }
    }
}

DeclRangeSet CollectAllDeclRanges(
    std::string_view symName, const std::vector<Symbol>& symGroup,
    const ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>& compatibleClasses,
    const SymbolTable& symbolTable)
{
    DeclRangeSet allDeclRanges;
    AddSymbolDeclRanges(symbolTable.FindSymbols(symName), allDeclRanges);
    for (const auto& c : compatibleClasses)
    {
        AddSymbolDeclRanges(symbolTable.FindMemberSymbols(c, symName), allDeclRanges);
    }
    for (const auto& s : symGroup)
    {
        if (!s.containerName.empty())
        {
            AddSymbolDeclRanges(symbolTable.FindMemberSymbols(s.containerName, symName), allDeclRanges);
        }
    }
    AddSymbolDeclRanges(symGroup, allDeclRanges);
    return allDeclRanges;
}

std::string ComputeTargetNamespace(bool isEnumConstant, std::string_view targetEnumContainer, const Symbol& sym,
                                   const SymbolTable& table)
{
    if (isEnumConstant)
    {
        return std::string(utils::ParentScopeView(targetEnumContainer));
    }
    if (!sym.containerName.empty() && !IsSymbolInClassContainer(sym, table))
    {
        return sym.containerName;
    }
    return {};
}

std::string ComputeEnumContainer(const Symbol& sym)
{
    if (std::holds_alternative<VariableSignature>(sym.signature))
    {
        const auto& var = sym.GetVariable();
        if (var.isEnumConstant)
        {
            return !var.typeName.empty() ? var.typeName : sym.containerName;
        }
    }
    return {};
}
} // namespace angel_lsp::analysis
