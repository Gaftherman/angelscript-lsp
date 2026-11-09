#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/types/TypeSanitization.h"
#include "document/Document.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Checks single base class properties (final, mixin class inherit).
 * @param[in] req Base check request context.
 * @param[in] base Base symbol to evaluate.
 * @param[in] cleanBase Sanitized base class name.
 * @return True if base is a concrete non-mixin class.
 */
static bool CheckBaseClassSymbol(const BaseCheckRequest& req, const Symbol& base, std::string_view cleanBase)
{
    if (base.type != SymbolType::Class)
    {
        return false;
    }

    if (!base.GetClass().modifiers.isMixin)
    {
        if (req.sig.modifiers.isMixin)
        {
            req.ctx.LogRule("CheckBases", "as-err-mixin-inherit-class", req.sym);
            req.ctx.Emit(req.sym, "as-err-mixin-inherit-class", req.sym.name, cleanBase);
        }
    }

    if (base.GetClass().modifiers.isFinal)
    {
        req.ctx.LogRule("CheckBases", "as-err-inherit-final", req.sym);
        req.ctx.Emit(req.sym, "as-err-inherit-final", cleanBase);
    }

    return !base.GetClass().modifiers.isMixin;
}

/**
 * @brief Collects the source spans of each entry in the base list of a class or interface symbol.
 * @param[in] sym Class or interface symbol whose line span locates its base_class_list.
 * @param[in] ctx Diagnostic context providing the document node index.
 * @return One span per base entry in declaration order; empty when the document has no index or no base list.
 */
static std::vector<IndexedSourceSpan> CollectBaseSpans(const Symbol& sym, const DiagnosticContext& ctx)
{
    return ctx.request.nodeIndex ? BuildBaseClassSpans(*ctx.request.nodeIndex, sym.startLine, sym.endLine)
                                 : std::vector<IndexedSourceSpan>{};
}

/**
 * @brief Selects the diagnostic range for the base at index, falling back when the base list facts are shorter.
 * @param[in] spans Base entry spans produced by CollectBaseSpans.
 * @param[in] index Position of the base in the signature base list.
 * @param[in] fallbackRange Range used when no span exists for index, normally the symbol selection range.
 * @return Range of the base entry, or fallbackRange.
 */
static SourceRange BaseRangeAt(std::span<const IndexedSourceSpan> spans, size_t index, const SourceRange& fallbackRange)
{
    if (index >= spans.size())
    {
        return fallbackRange;
    }
    const IndexedSourceSpan& span = spans[index];
    return SourceRange{span.startRow, span.startColumn, span.endRow, span.endColumn};
}

/**
 * @brief Checks if base class is synthetic or ignored by engine rules.
 * @param[in] cleanBase Sanitized base class name.
 * @param[in] ctx Diagnostic context.
 * @return True if base class is ignored or registered as a proxy.
 */
static bool IsIgnoredOrProxyBase(std::string_view cleanBase, const DiagnosticContext& ctx)
{
    if (!ctx.request.engineRules)
    {
        return false;
    }
    const auto& ignored = ctx.request.engineRules->unusedRules.ignoredBaseClasses;
    if (std::find(ignored.begin(), ignored.end(), cleanBase) != ignored.end())
    {
        return true;
    }
    return ctx.request.engineRules->unusedRules.baseClassProxyMap.contains(cleanBase);
}

/**
 * @brief Finds candidate base symbols in qualified, container, using-namespace or global scopes.
 * @param[in] cleanBase Sanitized base class name.
 * @param[in] sym Symbol providing container scope context.
 * @param[in] ctx Diagnostic context providing the symbol table.
 * @return Candidate base symbols or nullptr if unresolved.
 */
static std::shared_ptr<const std::vector<Symbol>>
FindBaseCandidateSymbols(std::string_view cleanBase, const Symbol& sym, const DiagnosticContext& ctx)
{
    const auto& table = ctx.request.symbolTable;

    std::shared_ptr<const std::vector<Symbol>> result = nullptr;
    utils::ForEachEnclosingScope(sym.containerName,
                                 [&](std::string_view scope)
                                 {
                                     if (!result)
                                     {
                                         auto syms = table.FindMemberSymbolPtr(scope, cleanBase);
                                         if (syms && !syms->empty())
                                         {
                                             result = std::move(syms);
                                         }
                                     }
                                 });
    if (result)
    {
        return result;
    }

    for (const auto& uNs : analysis::GetUsingNamespaces(ctx.request))
    {
        if (auto syms = table.FindMemberSymbolPtr(uNs, cleanBase))
        {
            if (!syms->empty())
            {
                return syms;
            }
        }
    }

    return table.FindSymbolsPtr(cleanBase);
}

/**
 * @brief Checks if a qualified base's prefix is an invalid non-namespace type like a class.
 * @param[in] cleanBase Sanitized base class name.
 * @param[in] sym Symbol being checked.
 * @param[in] ctx Diagnostic context.
 * @return True if qualifier is a known non-namespace type.
 */
static bool IsInvalidClassScopeQualifier(std::string_view cleanBase, const Symbol& sym, const DiagnosticContext& ctx)
{
    if (!HasScopeQualifier(cleanBase))
    {
        return false;
    }
    const std::string_view qual = utils::ParentScopeView(cleanBase);
    const auto qualSyms = FindBaseCandidateSymbols(qual, sym, ctx);
    if (!qualSyms || qualSyms->empty())
    {
        return false;
    }
    return std::none_of(qualSyms->begin(), qualSyms->end(),
                        [](const Symbol& s) { return s.type == SymbolType::Namespace; });
}

/**
 * @brief Validates an individual base class entry and returns true if it resolved to a concrete class base.
 * @param[in] req Base check request context.
 * @param[in] cleanBase Sanitized base class name.
 * @param[in] baseRange Source range of the base entry in source.
 * @return True if base resolved to a concrete non-mixin class.
 */
static bool ValidateOneBase(const BaseCheckRequest& req, std::string_view cleanBase, const SourceRange& baseRange)
{
    if (IsIgnoredOrProxyBase(cleanBase, req.ctx))
    {
        return false;
    }

    if (IsInvalidClassScopeQualifier(cleanBase, req.sym, req.ctx))
    {
        req.ctx.LogRule("CheckBases", "as-err-unresolved-type", req.sym);
        req.ctx.EmitAtRange(baseRange, "as-err-unresolved-type", cleanBase);
        return false;
    }

    const auto baseSymbols = FindBaseCandidateSymbols(cleanBase, req.sym, req.ctx);
    if (!baseSymbols || baseSymbols->empty())
    {
        req.ctx.LogRule("CheckBases", "as-err-unresolved-type", req.sym);
        req.ctx.EmitAtRange(baseRange, "as-err-unresolved-type", cleanBase);
        return false;
    }

    bool hasClassOrIface = false;
    bool sawClassBase = false;
    for (const auto& base : *baseSymbols)
    {
        if (base.type == SymbolType::Class)
        {
            hasClassOrIface = true;
            sawClassBase = CheckBaseClassSymbol(req, base, cleanBase);
            break;
        }
        if (base.type == SymbolType::Interface)
        {
            hasClassOrIface = true;
            break;
        }
    }

    if (!hasClassOrIface)
    {
        req.ctx.LogRule("CheckBases", "as-err-unresolved-type", req.sym);
        req.ctx.EmitAtRange(baseRange, "as-err-unresolved-type", cleanBase);
        return false;
    }

    return sawClassBase;
}

/**
 * @brief Rules about the base list: existence, kind, count and finality.
 */
void CheckBases(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    uint32_t classBaseCount = 0;
    const BaseCheckRequest req{sym, sig, ctx};
    const std::vector<IndexedSourceSpan> baseSpans = CollectBaseSpans(sym, ctx);

    for (size_t i = 0; i < sig.bases.size(); ++i)
    {
        const std::string_view cleanBase = CleanBaseTypeView(sig.bases[i]);
        if (cleanBase.empty())
        {
            continue;
        }

        const SourceRange baseRange = BaseRangeAt(baseSpans, i, sym.selectionRange);
        if (ValidateOneBase(req, cleanBase, baseRange))
        {
            if (!sig.modifiers.isMixin && ++classBaseCount > 1)
            {
                ctx.LogRule("CheckBases", "as-err-multi-class-inherit", sym);
                ctx.Emit(sym, "as-err-multi-class-inherit", sym.name);
            }
        }
    }

    if (HasInheritanceCycle(sym.qualifiedName.empty() ? std::string_view(sym.name)
                                                      : std::string_view(sym.qualifiedName),
                            ctx.request.symbolTable))
    {
        ctx.LogRule("CheckBases", "as-err-circular-inherit", sym);
        ctx.Emit(sym, "as-err-circular-inherit", sym.name);
    }
}

/**
 * @brief Validates bases declared on an interface.
 */
void CheckInterfaceBases(const Symbol& sym, const InterfaceSignature& sig, const DiagnosticContext& ctx)
{
    const std::vector<IndexedSourceSpan> baseSpans = CollectBaseSpans(sym, ctx);

    for (size_t i = 0; i < sig.inheritedInterfaces.size(); ++i)
    {
        const std::string_view cleanBase = CleanBaseTypeView(sig.inheritedInterfaces[i]);
        if (cleanBase.empty())
        {
            continue;
        }

        const SourceRange baseRange = BaseRangeAt(baseSpans, i, sym.selectionRange);
        if (IsIgnoredOrProxyBase(cleanBase, ctx))
        {
            continue;
        }

        if (IsInvalidClassScopeQualifier(cleanBase, sym, ctx))
        {
            ctx.LogRule("CheckInterfaceBases", "as-err-unresolved-type", sym);
            ctx.EmitAtRange(baseRange, "as-err-unresolved-type", cleanBase);
            continue;
        }

        const auto baseSymbols = FindBaseCandidateSymbols(cleanBase, sym, ctx);
        if (!baseSymbols || baseSymbols->empty())
        {
            ctx.LogRule("CheckInterfaceBases", "as-err-unresolved-type", sym);
            ctx.EmitAtRange(baseRange, "as-err-unresolved-type", cleanBase);
            continue;
        }

        bool hasInterfaceCandidate = false;
        for (const auto& b : *baseSymbols)
        {
            if (b.type == SymbolType::Interface)
            {
                hasInterfaceCandidate = true;
                break;
            }
        }

        if (!hasInterfaceCandidate)
        {
            ctx.LogRule("CheckInterfaceBases", "as-err-unresolved-type", sym);
            ctx.EmitAtRange(baseRange, "as-err-unresolved-type", cleanBase);
        }
    }
}

} // namespace angel_lsp::analysis::rules
