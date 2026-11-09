#include "analysis/rules/ClassRules.h"

#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "document/Document.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace angel_lsp::analysis::rules
{
void ValidateClass(const Symbol& sym, const DiagnosticContext& ctx, MixinFileCache* mixinCache)
{
    if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
    {
        return;
    }

    if (IsFromPredefinedStub(sym, ctx))
    {
        return;
    }

    if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) ||
        sym.name == ctx.request.GetEffectiveStringTypeName() || sym.name == ctx.request.GetEffectiveArrayTypeName())
    {
        ctx.LogRule("ValidateClass", "as-err-reserved-keyword-name", sym);
        ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
        return;
    }

    if (sym.type == SymbolType::Interface)
    {
        if (HasInheritanceCycle(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName, ctx.request.symbolTable))
        {
            ctx.LogRule("ValidateClass", "as-err-circular-inherit", sym);
            ctx.Emit(sym, "as-err-circular-inherit", sym.name);
        }
        CheckInterfaceBases(sym, sym.GetInterface(), ctx);
        return;
    }

    const auto& sig = sym.GetClass();
    if (sig.isTemplate)
    {
        ctx.LogRule("ValidateClass", "as-err-template-class-not-supported", sym);
        ctx.Emit(sym, "as-err-template-class-not-supported", sym.name);
    }

    CheckClassModifiers(sym, sig, ctx);
    CheckBases(sym, sig, ctx);
    CheckFinalOverrides(sym, sig, ctx);
    CheckInterfaceImplementation(sym, sig, ctx);
    CheckPropertyAccessors(sym, ctx);
    CheckMixinInstantiations(sym, sig, ctx, mixinCache);
}

} // namespace angel_lsp::analysis::rules
