#include "analysis/rules/PropertyRules.h"

namespace angel_lsp::analysis::rules
{
    static bool Rule_PropertyModifiers(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetVariable();

        if (!sym.containerName.empty() && (sig.isVirtualProperty || sig.hasGet || sig.hasSet))
        {
            const auto *containerSyms = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
            if (containerSyms)
            {
                for (const auto &cSym : *containerSyms)
                {
                    if (cSym.type == SymbolType::Class && cSym.GetClass().modifiers.isMixin)
                    {
                        ctx.LogRule("Rule_PropertyModifiers", "as-err-mixin-virtual-property", sym);
                        ctx.Emit(sym, "as-err-mixin-virtual-property");
                        return true;
                    }
                }
            }
        }

        if (sig.typeKind == TypeKind::Void)
        {
            ctx.LogRule("Rule_PropertyModifiers", "as-err-void-variable", sym);
            ctx.Emit(sym, "as-err-void-variable");
        }

        if (!sym.containerName.empty() && sig.modifiers.isConst)
        {
            const auto *containerSyms = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
            if (containerSyms)
            {
                for (const auto &cSym : *containerSyms)
                {
                    if (cSym.type == SymbolType::Class)
                    {
                        ctx.LogRule("Rule_PropertyModifiers", "as-err-class-member-const", sym);
                        ctx.Emit(sym, "as-err-class-member-const", sym.name);
                        break;
                    }
                }
            }
        }

        if (sig.hasPrimitiveHandle)
        {
            ctx.LogRule("Rule_PropertyModifiers", "as-err-handle-on-primitive", sym);
            ctx.Emit(sym, "as-err-handle-on-primitive", sig.baseTypeName);
        }

        if (sig.typeKind == TypeKind::Unknown && !sig.baseTypeName.empty())
        {
            if (!ctx.request.symbolTable.HasSymbol(sig.baseTypeName))
            {
                ctx.LogRule("Rule_PropertyModifiers", "as-err-unresolved-type", sym);
                ctx.Emit(sym, "as-err-unresolved-type", sig.baseTypeName);
            }
        }

        return false;
    }

    static void Rule_PropertyAccessors(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetVariable();

        bool isInterfaceProperty = false;
        if (!sym.containerName.empty())
        {
            const auto *containerSyms = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
            if (containerSyms)
            {
                for (const auto &cSym : *containerSyms)
                {
                    if (cSym.type == SymbolType::Interface)
                    {
                        isInterfaceProperty = true;
                        break;
                    }
                }
            }
        }

        if (sym.containerName.empty() && (sig.modifiers.isProperty || sig.isVirtualProperty || sig.hasGet || sig.hasSet))
        {
            ctx.LogRule("Rule_PropertyAccessors", "as-err-global-function-qualifiers", sym);
            ctx.Emit(sym, "as-err-global-function-qualifiers", sym.name);
        }

        if (isInterfaceProperty && (sig.hasBodyGet || sig.hasBodySet))
        {
            ctx.LogRule("Rule_PropertyAccessors", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (!isInterfaceProperty && ((sig.hasGet && !sig.hasBodyGet) || (sig.hasSet && !sig.hasBodySet)))
        {
            ctx.LogRule("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
            ctx.Emit(sym, "as-err-property-accessor-missing-body", sym.name);
        }

        if (sig.hasDuplicateGet || sig.hasDuplicateSet)
        {
            ctx.LogRule("Rule_PropertyAccessors", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (isInterfaceProperty && (sig.isGetFinal || sig.isGetOverride || sig.isSetFinal || sig.isSetOverride))
        {
            ctx.LogRule("Rule_PropertyAccessors", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (!isInterfaceProperty && (sig.isGetOverride || sig.isSetOverride))
        {
            bool hasBaseProperty = false;
            auto parentOpt = ctx.request.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && parentOpt->type == SymbolType::Class)
            {
                for (const auto &bName : parentOpt->GetClass().bases)
                {
                    auto bSyms = ctx.request.symbolTable.FindSymbolsPtr(bName);
                    if (bSyms)
                    {
                        for (const auto &bSym : *bSyms)
                        {
                            std::string propQN = bSym.qualifiedName.empty() ? sym.name : bSym.qualifiedName + "::" + sym.name;
                            if (ctx.request.symbolTable.HasSymbol(propQN))
                            {
                                hasBaseProperty = true;
                                break;
                            }
                        }
                    }
                    if (hasBaseProperty) break;
                }
            }
            if (!hasBaseProperty)
            {
                ctx.LogRule("Rule_PropertyAccessors", "as-err-override-no-base", sym);
                ctx.Emit(sym, "as-err-override-no-base", sym.name, sym.containerName);
            }
        }

        if (!isInterfaceProperty && sig.modifiers.isProperty && !sym.GetVariable().modifiers.isExternal)
        {
            const auto *funcSyms = ctx.request.symbolTable.FindSymbolsPtr(sym.name);
            if (funcSyms)
            {
                for (const auto &fSym : *funcSyms)
                {
                    if (fSym.fileUri == sym.fileUri && fSym.startLine == sym.startLine &&
                        fSym.type == SymbolType::Function && !fSym.GetFunction().hasBody)
                    {
                        ctx.LogRule("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
                        ctx.Emit(sym, "as-err-property-accessor-missing-body", sym.name);
                        break;
                    }
                }
            }
        }
    }

    void ValidateProperty(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (Rule_PropertyModifiers(sym, ctx))
        {
            return;
        }

        Rule_PropertyAccessors(sym, ctx);
    }
}
