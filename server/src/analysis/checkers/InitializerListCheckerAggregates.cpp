#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Inspects a member symbol and records it if it represents an aggregate field or list constructor.
 * @param[in] s        Member symbol.
 * @param[in] classSym Containing class symbol.
 * @param[in,out] info Aggregate struct info sink.
 */
static void InspectStructMemberSymbol(const Symbol& s, const Symbol& classSym, AggregateStructInfo& info)
{
    if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
    {
        if (std::holds_alternative<VariableSignature>(s.signature))
        {
            const auto& varSig = s.GetVariable();
            if (!varSig.isVirtualProperty)
            {
                const std::string_view fType = varSig.baseTypeName.empty() ? std::string_view(varSig.typeName)
                                                                           : std::string_view(varSig.baseTypeName);
                info.fields.push_back({s.name, StripDecorations(fType), s.startLine, s.startCharacter});
            }
        }
    }
    else if (s.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(s.signature))
    {
        if (s.name == classSym.name && IsListConstructorSignature(s.GetFunction()))
        {
            info.hasListSupport = true;
        }
    }
}

/**
 * @brief Collects aggregate struct fields and list constructors from a container key.
 * @param[in] container Container name key in rule index.
 * @param[in] classSym  Containing class symbol.
 * @param[in] ctx       Diagnostic context.
 * @param[in,out] info  Aggregate struct info sink.
 */
static void CollectStructContainerMembers(std::string_view container, const Symbol& classSym,
                                          const DiagnosticContext& ctx, AggregateStructInfo& info)
{
    const auto& members = ctx.request.GetRuleIndex().Members(container);
    for (const auto& key : members.memberKeys)
    {
        if (const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key))
        {
            for (const auto& s : *mSyms)
            {
                InspectStructMemberSymbol(s, classSym, info);
            }
        }
    }
}

/**
 * @brief Sorts and deduplicates struct field definitions by declaration order.
 * @param[in,out] fields Vector of struct fields to finalize.
 */
static void FinalizeStructFields(std::vector<StructFieldInfo>& fields)
{
    std::sort(fields.begin(), fields.end(),
              [](const StructFieldInfo& a, const StructFieldInfo& b)
              {
                  if (a.line != b.line)
                  {
                      return a.line < b.line;
                  }
                  if (a.col != b.col)
                  {
                      return a.col < b.col;
                  }
                  return a.name < b.name;
              });

    fields.erase(std::unique(fields.begin(), fields.end(), [](const StructFieldInfo& a, const StructFieldInfo& b)
                             { return a.name == b.name && a.line == b.line && a.col == b.col; }),
                 fields.end());
}

/**
 * @brief Inspects a single class symbol for aggregate struct fields and list constructors.
 * @param[in] sym      Class symbol to inspect.
 * @param[in] spelling Parsed template spelling.
 * @param[in] ctx      Diagnostic context.
 * @param[in,out] info Aggregate struct info sink.
 */
static void InspectClassAggregate(const Symbol& sym, const TemplateSpelling& spelling, const DiagnosticContext& ctx,
                                  AggregateStructInfo& info)
{
    if (sym.type != SymbolType::Class)
    {
        return;
    }

    if (ctx.request.IsRegisteredSymbol(sym.name) ||
        (!sym.qualifiedName.empty() && ctx.request.IsRegisteredSymbol(sym.qualifiedName)) ||
        IsFromPredefinedStub(sym, ctx))
    {
        info.hasListSupport = true;
    }

    const auto candidateContainers = CandidateContainerKeys(sym, spelling.name);
    for (const auto& container : candidateContainers)
    {
        CollectStructContainerMembers(container, sym, ctx, info);
    }
}

AggregateStructInfo InspectAggregateStruct(const TemplateSpelling& spelling, const DiagnosticContext& ctx,
                                           StructLayoutCache& cache)
{
    if (spelling.name.empty())
    {
        return {};
    }

    const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name);
    const auto typeSymbols = syms ? *syms : ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);

    const Symbol* classSym = nullptr;
    for (const auto& sym : typeSymbols)
    {
        if (sym.type == SymbolType::Class)
        {
            classSym = &sym;
            break;
        }
    }

    const std::string cacheKey = classSym ? (classSym->qualifiedName.empty() ? classSym->name : classSym->qualifiedName)
                                          : std::string(spelling.name);

    const auto it = cache.find(cacheKey);
    if (it != cache.end())
    {
        return it->second;
    }

    AggregateStructInfo info;
    if (ctx.request.IsRegisteredSymbol(spelling.name))
    {
        info.hasListSupport = true;
    }

    for (const auto& sym : typeSymbols)
    {
        InspectClassAggregate(sym, spelling, ctx, info);
    }

    FinalizeStructFields(info.fields);
    cache.emplace(cacheKey, info);
    return info;
}

void ValidateStructAggregateList(const InitializerListFact& list, std::string_view type,
                                 const AggregateStructInfo& structInfo, const ListValidationContext& valCtx)
{
    if (!structInfo.hasListSupport)
    {
        valCtx.ctx.EmitAtRange(list.range, "as-err-initializer-list-not-supported", type);
        return;
    }

    const uint32_t written = list.valueCount;
    const auto expected = static_cast<uint32_t>(structInfo.fields.size());
    if (written < expected)
    {
        valCtx.ctx.EmitAtRange(list.range, "as-err-initializer-list-too-few", "");
        return;
    }
    if (written > expected)
    {
        valCtx.ctx.EmitAtRange(list.range, "as-err-initializer-list-too-many", "");
        return;
    }

    const ListValidationContext nextCtx{valCtx.ctx, valCtx.elements, valCtx.cache, valCtx.facts, valCtx.depth + 1};
    size_t fieldIdx = 0;
    for (const auto& elem : valCtx.facts.Elements(list))
    {
        if (fieldIdx >= structInfo.fields.size())
        {
            return;
        }
        if (elem.kind == InitializerListElementKind::NestedList && elem.nestedListIndex != NoInitializerList)
        {
            InitializerListValidator::ValidateList(valCtx.facts.lists[elem.nestedListIndex],
                                                   structInfo.fields[fieldIdx].type, nextCtx);
        }
        else
        {
            CheckElementValue(elem, structInfo.fields[fieldIdx].type, valCtx.ctx, valCtx.elements);
        }
        ++fieldIdx;
    }
}

} // namespace angel_lsp::analysis
