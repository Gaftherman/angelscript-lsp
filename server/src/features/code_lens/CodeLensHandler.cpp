#include "features/code_lens/CodeLensHandler.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/MixinInclusion.h"
#include "analysis/resolution/ReferenceCounting.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/rules/UnusedSymbolChecker.h"
#include "features/code_lens/CodeLensHandlerInternal.h"
#include "utils/AccessorNames.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"

#include <algorithm>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <vector>

namespace angel_lsp::features
{
static void AppendReferenceLens(const analysis::SourceRange& key, const analysis::Symbol& sym,
                                const AssembledLensContext& ctx, std::vector<lsp::CodeLens>& lenses)
{
    auto it = ctx.keyToTarget.find(key);
    if (it == ctx.keyToTarget.end())
    {
        return;
    }
    size_t refCount = ctx.targets[it->second].seenRefs.size();
    if (ctx.request.unusedRules &&
        analysis::rules::IsEngineManagedFunction(sym, ctx.request.symbolTable, *ctx.request.unusedRules))
    {
        refCount += 1;
    }
    if (refCount == 0 && ctx.request.config && !ctx.request.config->features.codeLensShowZeroReferences)
    {
        return;
    }
    if (refCount == 0 && sym.type == analysis::SymbolType::Function && analysis::rules::IsOperatorName(sym.name))
    {
        return;
    }
    auto lens = MakeCommandLens(key, analysis::FormatReferenceCount(refCount, ctx.request.i18n));
    if (!ctx.request.config || ctx.request.config->features.enableReferences)
    {
        const auto& selection = sym.selectionRange == analysis::SourceRange{} ? key : sym.selectionRange;
        const auto line = ctx.lines.Line(ctx.request.sourceCode, selection.startLine);
        const auto character =
            utils::ByteToLspCharColumn(line, selection.startCharacter, utils::PositionEncoding::Utf16);
        const auto uri = ctx.request.clientUri.empty() ? ctx.request.uri : ctx.request.clientUri;
        lsp::json::Array arguments;
        arguments.emplace_back(MakeJsonString(uri));
        arguments.emplace_back(static_cast<lsp::json::Integer>(selection.startLine));
        arguments.emplace_back(static_cast<lsp::json::Integer>(character));
        lens.command->command = "angelscript.showReferences";
        lens.command->arguments = std::move(arguments);
    }
    lenses.push_back(std::move(lens));
}

static void AppendFunctionLens(const analysis::SourceRange& key, const analysis::Symbol& sym,
                               const AssembledLensContext& ctx, std::vector<lsp::CodeLens>& lenses)
{
    if (analysis::IsDestructorDeclaration(sym, ctx.request.sourceCode))
    {
        return;
    }
    if (analysis::IsInterfaceMethod(sym, ctx.request.symbolTable))
    {
        size_t implCount = analysis::CountInterfaceMethodImplementations(sym, ctx.request.symbolTable);
        lenses.push_back(MakeCommandLens(key, analysis::FormatImplementationCount(implCount, ctx.request.i18n)));
        return;
    }
    AppendReferenceLens(key, sym, ctx, lenses);
}

static void AppendLensesForGroup(const analysis::SourceRange& key, const std::vector<analysis::Symbol>& symGroup,
                                 const AssembledLensContext& ctx, std::vector<lsp::CodeLens>& lenses)
{
    const auto& sym = SelectPrimarySymbol(symGroup);
    if (sym.type == analysis::SymbolType::Function)
    {
        AppendFunctionLens(key, sym, ctx, lenses);
    }
    else if (sym.type == analysis::SymbolType::Interface)
    {
        size_t implCount = analysis::CountInterfaceImplementations(sym, ctx.request.symbolTable);
        lenses.push_back(MakeCommandLens(key, analysis::FormatImplementationCount(implCount, ctx.request.i18n)));
    }
    else if (sym.type == analysis::SymbolType::Class)
    {
        AppendReferenceLens(key, sym, ctx, lenses);
    }
    else if (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property)
    {
        AppendReferenceLens(key, sym, ctx, lenses);
    }
}

static std::vector<lsp::CodeLens> BuildSymbolGroupLenses(const SymbolGroups& symGroups, const CodeLensRequest& request)
{
    analysis::MixinClassContext mixinCtx{request.symbolTable, analysis::BuildMixinInclusionMap(request.symbolTable)};

    std::vector<analysis::ReferenceCountTarget> targets;
    KeyTargetMap keyToTarget;
    TargetCollectionContext targetCtx{mixinCtx, request.sourceCode, keyToTarget};
    CollectReferenceTargets(symGroups, targetCtx, targets);

    if (!targets.empty())
    {
        analysis::TargetIndexMap targetsByName;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            targetsByName[targets[i].targetName].push_back(i);
            if (const std::string_view propertyName = utils::AccessorPropertyName(targets[i].targetName);
                !propertyName.empty())
            {
                targetsByName[propertyName].push_back(i);
            }
        }
        analysis::ReferenceCountRequest refReq{
            .uri = request.uri,
            .sourceCode = request.sourceCode,
            .symbolTable = request.symbolTable,
            .scopeIndex = request.scopeIndex,
            .predefinedExtension = request.predefinedExtension,
            .occurrenceModule = request.occurrenceModule,
        };
        analysis::CountReferences(targets, targetsByName, refReq);
    }

    std::vector<lsp::CodeLens> lenses;
    AssembledLensContext assembledCtx{targets, keyToTarget, request, utils::LineIndex::Build(request.sourceCode)};
    for (const auto& key : symGroups.rangeOrder)
    {
        auto it = symGroups.groups.find(key);
        if (it != symGroups.groups.end() && !it->second.empty())
        {
            AppendLensesForGroup(key, it->second, assembledCtx, lenses);
        }
    }
    return lenses;
}

std::optional<std::vector<lsp::CodeLens>> GetCodeLenses(const CodeLensRequest& request)
{
    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[CodeLens] Computing code lenses for URI: {}", request.uri));
    }

    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    auto symGroups = CollectSymbolGroups(request);
    if (symGroups.rangeOrder.empty())
    {
        return std::nullopt;
    }

    auto lenses = BuildSymbolGroupLenses(symGroups, request);
    if (lenses.empty())
    {
        return std::nullopt;
    }

    if (request.logger && request.logger->IsTraceEnabled())
    {
        request.logger->LogTrace(
            fmt::format("[CodeLens] Generated {} code lenses for URI: {}", lenses.size(), request.uri));
    }

    return lenses;
}

std::optional<lsp::CodeLens> ResolveCodeLens(const CodeLensResolveRequest& request)
{
    if (request.logger && request.logger->IsTraceEnabled())
    {
        request.logger->LogTrace("[CodeLens] Resolving code lens");
    }
    return request.codeLens;
}
} // namespace angel_lsp::features
