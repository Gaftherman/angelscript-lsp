#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Inspects a member symbol and records it if it represents an aggregate field or list constructor.
 * @param[in] s        Member symbol.
 * @param[in] classSym Containing class symbol.
 * @param[in,out] info     Aggregate struct info sink.
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
                std::string fType = varSig.baseTypeName.empty() ? varSig.typeName : varSig.baseTypeName;
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
 * @param[in,out] info      Aggregate struct info sink.
 */
static void CollectStructContainerMembers(const std::string& container, const Symbol& classSym,
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
 * @param[in,out] info     Aggregate struct info sink.
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

/**
 * @brief Dynamically extracts member properties in declaration order for an aggregate struct/class.
 * @param[in] spelling Parsed template spelling of the struct.
 * @param[in] ctx      Diagnostic context.
 * @param[in,out] cache    Layout cache across inspections.
 * @return Aggregate struct field layout and list initialization support.
 */
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

    const std::string cacheKey =
        classSym ? (classSym->qualifiedName.empty() ? classSym->name : classSym->qualifiedName) : spelling.name;

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

/**
 * @brief Validates an initializer list against an aggregate struct or class layout.
 * @param[in] listNode   Initializer list node.
 * @param[in] type       Decorated or unadorned type name.
 * @param[in] structInfo Struct layout information.
 * @param[in] valCtx     Validation context.
 */
void ValidateStructAggregateList(TSNode listNode, const std::string& type, const AggregateStructInfo& structInfo,
                                 const ListValidationContext& valCtx)
{
    if (!structInfo.hasListSupport)
    {
        valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(listNode), "as-err-initializer-list-not-supported", type);
        return;
    }

    const uint32_t written = ListValueCount(listNode);
    const auto expected = static_cast<uint32_t>(structInfo.fields.size());
    if (written < expected)
    {
        valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(listNode), "as-err-initializer-list-too-few", "");
        return;
    }
    if (written > expected)
    {
        valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(listNode), "as-err-initializer-list-too-many", "");
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const ListValidationContext nextCtx{valCtx.ctx, valCtx.elements, valCtx.cache, valCtx.depth + 1};
    size_t fieldIdx = 0;
    parser::ForEachChildNode(
        listNode,
        [&](TSNode elem)
        {
            if (!ts_node_is_named(elem) || ts_node_symbol(elem) == gs.symComment)
            {
                return;
            }
            if (fieldIdx >= structInfo.fields.size())
            {
                return;
            }
            const TSSymbol sym = ts_node_symbol(elem);
            if (sym == gs.symInitializerList)
            {
                InitializerListValidator::ValidateList(elem, structInfo.fields[fieldIdx].type, nextCtx);
            }
            else
            {
                CheckElementValue(elem, structInfo.fields[fieldIdx].type, valCtx.ctx, valCtx.elements);
            }
            ++fieldIdx;
        });
}
} // namespace angel_lsp::analysis
