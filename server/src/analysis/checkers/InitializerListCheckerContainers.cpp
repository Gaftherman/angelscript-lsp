#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <optional>
#include <string>
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
 * @param[out] out          Inspection state sink.
 */
static void InspectContainerMembers(const std::string& containerKey, const std::string& className,
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

    auto resolveElem = [&](const std::string& fallback)
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

/**
 * @brief Inspects whether a type represents a sequence or multi-dimensional container.
 *        Identifies containers via:
 * @param[in] type               Cleaned type string.
 * @param[in] spelling           Parsed template spelling.
 * @param[in] ctx                Diagnostic context.
 * @return Resolved container dimensionality and element type.
 */
ListContainerInfo InspectContainer(const std::string& type, const TemplateSpelling& spelling,
                                   const DiagnosticContext& ctx)
{
    if (type.ends_with("[]"))
    {
        return {true, 1, type.substr(0, type.size() - 2)};
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

/**
 * @brief Validates elements of a sequence or multi-dimensional container recursively.
 * @param[in] node      Initializer list node.
 * @param[in] dimension Remaining container dimensions.
 * @param[in] elemType  Expected element type.
 * @param[in] valCtx    Validation context.
 */
void ValidateMultiDimensionalContainer(TSNode node, uint32_t dimension, const std::string& elemType,
                                       const ListValidationContext& valCtx)
{
    if (ts_node_is_null(node))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const ListValidationContext nextCtx{valCtx.ctx, valCtx.elements, valCtx.cache, valCtx.depth + 1};
    parser::ForEachChildNode(node,
                             [&](TSNode child)
                             {
                                 if (!ts_node_is_named(child) || ts_node_symbol(child) == gs.symComment)
                                 {
                                     return;
                                 }
                                 const TSSymbol sym = ts_node_symbol(child);
                                 if (dimension > 1)
                                 {
                                     if (sym != gs.symInitializerList)
                                     {
                                         valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(child),
                                                                    "as-err-initializer-list-expected", "");
                                     }
                                     else
                                     {
                                         ValidateMultiDimensionalContainer(child, dimension - 1, elemType, nextCtx);
                                     }
                                 }
                                 else
                                 {
                                     if (sym == gs.symInitializerList)
                                     {
                                         InitializerListValidator::ValidateList(child, elemType, nextCtx);
                                     }
                                     else
                                     {
                                         CheckElementValue(child, elemType, valCtx.ctx, valCtx.elements);
                                     }
                                 }
                             });
}
} // namespace angel_lsp::analysis
