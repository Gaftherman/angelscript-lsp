#include "analysis/rules/FunctionRules.h"

#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleSyntaxFacts.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/AccessorNames.h"
#include "utils/Utils.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
static void ValidateNonMemberPropertyAccessors(const Symbol& sym, const FunctionSignature& sig,
                                               const DiagnosticContext& ctx)
{
    if (!sym.name.starts_with("set_") || sig.parameters.empty())
    {
        return;
    }
    const std::string_view propName = std::string_view(sym.name).substr(4);
    if (propName.empty())
    {
        return;
    }
    std::array<char, 512> getterBuf{};
    const std::string_view getterName = utils::ComposeGetterName(propName, getterBuf);
    if (getterName.empty())
    {
        return;
    }
    std::shared_ptr<const std::vector<Symbol>> getSyms;
    if (sym.containerName.empty() || sym.containerKind == ContainerKind::None)
    {
        getSyms = ctx.request.symbolTable.FindSymbolsPtr(getterName);
    }
    else
    {
        getSyms = ctx.request.symbolTable.FindMemberSymbolPtr(sym.containerName, getterName);
    }
    if (!getSyms || getSyms->empty())
    {
        return;
    }
    const std::string_view setParam = CleanBaseTypeView(sig.parameters.back().typeName);
    for (const auto& gs : *getSyms)
    {
        if (gs.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(gs.signature))
        {
            const std::string_view getRet = CleanBaseTypeView(gs.GetFunction().returnType);
            if (!getRet.empty() && !setParam.empty() && getRet != setParam)
            {
                ctx.LogRule("CheckPropertyAccessors", "as-err-property-type-mismatch", sym);
                ctx.Emit(sym, "as-err-property-type-mismatch", {propName, getRet, setParam});
                break;
            }
        }
    }
}

void ValidateFunction(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Function)
    {
        return;
    }
    if (IsFromPredefinedStub(sym, ctx))
    {
        return;
    }
    if (!std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return;
    }

    const auto& sig = sym.GetFunction();
    const FunctionContext fctx = BuildFunctionContext(sym, ctx);

    CheckBody(sym, sig, fctx, ctx);
    CheckExternal(sym, sig, ctx);
    CheckReturnType(sym, sig, fctx, ctx);
    CheckModifiers(sym, sig, fctx, ctx);
    CheckAttributes(sym, sig, fctx, ctx);
    CheckConstructorDestructor(sym, sig, fctx, ctx);
    CheckOverride(sym, sig, fctx, ctx);
    ValidateParameters(sym, sig.parameters, false, ctx);

    // Not on an interface method, and this is not a refinement - it is the difference between
    // a hint and a trap. AngelScript's parser refuses `property` on an interface method
    // outright ("Expected ';'", verified against angelscript_oracle), so hinting there would be
    // advice whose only fix does not compile. The mode-3 rejection such an interface produces
    // lands on the IMPLEMENTING class's accessor, which this rule reaches on its own.
    if (ctx.request.diagnostics && ctx.request.diagnostics->reportAccessorPortability &&
        (!ctx.request.engineProperties || ctx.request.engineProperties->propertyAccessorMode != 2) && fctx.isMember &&
        !sig.isInterfaceMethod && !sig.modifiers.isProperty)
    {
        std::string_view accessor = sym.name;
        if (accessor.starts_with("get_") || accessor.starts_with("set_"))
        {
            accessor.remove_prefix(4);
            if (!accessor.empty())
            {
                ctx.LogRule("ValidateFunction", "as-hint-accessor-portability", sym);
                ctx.Emit(sym, "as-hint-accessor-portability", accessor, DiagnosticSeverity::Hint);
            }
        }
    }

    if (!fctx.isMember)
    {
        ValidateNonMemberPropertyAccessors(sym, sig, ctx);
    }
}

/**
 * @brief Reports an anonymous function (lambda expression) using pre-indexed AST facts.
 * @param[in] nodeIndex Pre-indexed syntax tree nodes.
 * @param[in] ctx Diagnostic context.
 */
void ValidateStandaloneLambda(const analysis::NodeIndex& nodeIndex, const DiagnosticContext& ctx)
{
    for (const auto& span : BuildRuleSyntaxFacts(nodeIndex).standaloneLambdas)
    {
        ctx.EmitAtRange({span.startRow, span.startColumn, span.endRow, span.endColumn},
                        diagnostics::codes::StandaloneAnonymousFunction, DiagnosticSeverity::Error);
    }
}

} // namespace angel_lsp::analysis::rules
