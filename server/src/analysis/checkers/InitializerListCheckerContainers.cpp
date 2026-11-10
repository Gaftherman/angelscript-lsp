#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Inspects container member functions for multi-index operators and list constructors.
 * @param[in] containerKey Class key in rule index.
 * @param[in] className    Short name of the container class.
 * @param[in] ctx          Diagnostic context.
 * @param[out] out         Inspection state sink.
 */
static void InspectContainerMembers(std::string_view containerKey, std::string_view className,
                                    const DiagnosticContext& ctx, ContainerMemberInspection& out)
{
    const auto& members = ctx.request.GetRuleIndex().Members(containerKey);
    for (const auto& key : members.memberKeys)
    {
        const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key);
        if (!mSyms)
        {
            continue;
        }
        for (const auto& m : *mSyms)
        {
            if (m.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(m.signature))
            {
                continue;
            }
            const auto& fn = m.GetFunction();
            if (m.name == "opIndex")
            {
                if (fn.parameters.size() > out.maxIndexParams)
                {
                    out.maxIndexParams = static_cast<uint32_t>(fn.parameters.size());
                    out.indexReturnType = fn.returnBaseTypeName.empty() ? fn.returnType : fn.returnBaseTypeName;
                }
            }
            else if (m.name == className && IsListConstructorSignature(fn))
            {
                out.hasListConstructor = true;
            }
        }
    }
}

/**
 * @brief Inspects a class symbol to determine whether it behaves as a container.
 * @param[in] sym      Class symbol to inspect.
 * @param[in] spelling Template spelling of the container type.
 * @param[in] ctx      Diagnostic context.
 * @return Container info if class behaves as a container, std::nullopt otherwise.
 */
static std::optional<ListContainerInfo> InspectClassContainer(const Symbol& sym, const TemplateSpelling& spelling,
                                                              const DiagnosticContext& ctx)
{
    ContainerMemberInspection inspection;
    const auto containers = CandidateContainerKeys(sym, spelling.name);
    for (const auto& containerKey : containers)
    {
        InspectContainerMembers(containerKey, sym.name, ctx, inspection);
    }

    auto resolveElem = [&](std::string_view fallback) -> std::string
    {
        std::string elem = !spelling.arguments.empty() ? spelling.arguments[0] : StripDecorations(fallback);
        return elem.empty() ? "auto" : elem;
    };

    if (inspection.maxIndexParams >= 2)
    {
        return ListContainerInfo{true, inspection.maxIndexParams, resolveElem(inspection.indexReturnType)};
    }
    if (inspection.hasListConstructor && (!spelling.arguments.empty() || inspection.maxIndexParams == 1))
    {
        return ListContainerInfo{true, 1, resolveElem(inspection.indexReturnType)};
    }
    return std::nullopt;
}

ListContainerInfo InspectContainer(std::string_view type, const TemplateSpelling& spelling,
                                   const DiagnosticContext& ctx)
{
    if (type.ends_with("[]"))
    {
        return {true, 1, std::string(type.substr(0, type.size() - 2))};
    }

    if (ctx.request.IsArrayLikeTemplate(spelling.name))
    {
        std::string elem = spelling.arguments.empty() ? "auto" : spelling.arguments[0];
        return {true, 1, std::move(elem)};
    }

    if (!spelling.name.empty())
    {
        const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name);
        const auto typeSymbols = syms ? *syms : ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);
        for (const auto& sym : typeSymbols)
        {
            if (sym.type == SymbolType::Class)
            {
                if (auto info = InspectClassContainer(sym, spelling, ctx))
                {
                    return *info;
                }
            }
        }
    }

    return {false, 1, ""};
}

void ValidateMultiDimensionalContainer(const InitializerListFact& list, uint32_t dimension, std::string_view elemType,
                                       const ListValidationContext& valCtx)
{
    const ListValidationContext nextCtx{valCtx.ctx, valCtx.elements, valCtx.cache, valCtx.facts, valCtx.depth + 1};
    for (const auto& elem : valCtx.facts.Elements(list))
    {
        if (dimension > 1)
        {
            if (elem.kind != InitializerListElementKind::NestedList || elem.nestedListIndex == NoInitializerList)
            {
                valCtx.ctx.EmitAtRange(elem.range, "as-err-initializer-list-expected", "");
            }
            else
            {
                ValidateMultiDimensionalContainer(valCtx.facts.lists[elem.nestedListIndex], dimension - 1, elemType,
                                                  nextCtx);
            }
        }
        else
        {
            if (elem.kind == InitializerListElementKind::NestedList && elem.nestedListIndex != NoInitializerList)
            {
                InitializerListValidator::ValidateList(valCtx.facts.lists[elem.nestedListIndex], elemType, nextCtx);
            }
            else
            {
                CheckElementValue(elem, elemType, valCtx.ctx, valCtx.elements);
            }
        }
    }
}

} // namespace angel_lsp::analysis
