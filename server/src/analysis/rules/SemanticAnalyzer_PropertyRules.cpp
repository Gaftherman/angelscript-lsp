#include "analysis/SemanticAnalyzerInternal.h"

namespace angel_lsp::analysis
{
    bool SemanticAnalyzer::Rule_PropertyModifiers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetVariable();

        if (!sym.containerName.empty() && (sig.isVirtualProperty || sig.hasGet || sig.hasSet))
        {
            const auto *containerSyms = req.symbolTable.FindSymbolsPtr(sym.containerName);
            if (containerSyms)
            {
                for (const auto &cSym : *containerSyms)
                {
                    if (cSym.type == SymbolType::Class && cSym.GetClass().modifiers.isMixin)
                    {
                        DebugDiag("Rule_PropertyModifiers", "as-err-mixin-virtual-property", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-virtual-property"));
                        return true;
                    }
                }
            }
        }

        if (sig.typeKind == TypeKind::Void)
        {
            DebugDiag("Rule_PropertyModifiers", "as-err-void-variable", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-variable"));
        }

        if (!sym.containerName.empty() && sig.modifiers.isConst)
        {
            const auto *containerSyms = req.symbolTable.FindSymbolsPtr(sym.containerName);
            if (containerSyms)
            {
                for (const auto &cSym : *containerSyms)
                {
                    if (cSym.type == SymbolType::Class)
                    {
                        DebugDiag("Rule_PropertyModifiers", "as-err-class-member-const", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-class-member-const", sym.name));
                        break;
                    }
                }
            }
        }

        if (sig.hasPrimitiveHandle)
        {
            DebugDiag("Rule_PropertyModifiers", "as-err-handle-on-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
        }

        if (sig.typeKind == TypeKind::Unknown && !sig.baseTypeName.empty())
        {
            if (!req.symbolTable.HasSymbol(sig.baseTypeName))
            {
                DebugDiag("Rule_PropertyModifiers", "as-err-unresolved-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.baseTypeName));
            }
        }

        return false;
    }

    void SemanticAnalyzer::Rule_PropertyAccessors(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetVariable();

        bool isInterfaceProperty = false;
        if (!sym.containerName.empty())
        {
            const auto *containerSyms = req.symbolTable.FindSymbolsPtr(sym.containerName);
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
            DebugDiag("Rule_PropertyAccessors", "as-err-global-function-qualifiers", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-function-qualifiers", sym.name));
        }

        if (isInterfaceProperty && (sig.hasBodyGet || sig.hasBodySet))
        {
            DebugDiag("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (!isInterfaceProperty && ((sig.hasGet && !sig.hasBodyGet) || (sig.hasSet && !sig.hasBodySet)))
        {
            DebugDiag("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (sig.hasDuplicateGet || sig.hasDuplicateSet)
        {
            DebugDiag("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (isInterfaceProperty && (sig.isGetFinal || sig.isGetOverride || sig.isSetFinal || sig.isSetOverride))
        {
            DebugDiag("Rule_PropertyAccessors", "as-syntax-error", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (!isInterfaceProperty && (sig.isGetOverride || sig.isSetOverride))
        {
            bool hasBaseProperty = false;
            auto parentOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && parentOpt->type == SymbolType::Class)
            {
                for (const auto &bName : parentOpt->GetClass().bases)
                {
                    auto bSyms = req.symbolTable.FindSymbolsPtr(bName);
                    if (bSyms)
                    {
                        for (const auto &bSym : *bSyms)
                        {
                            std::string propQN = bSym.qualifiedName.empty() ? sym.name : bSym.qualifiedName + "::" + sym.name;
                            if (req.symbolTable.HasSymbol(propQN))
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
                DebugDiag("Rule_PropertyAccessors", "as-err-override-no-base", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-override-no-base", sym.name, sym.containerName));
            }
        }

        if (!isInterfaceProperty && sig.modifiers.isProperty && !sym.GetVariable().modifiers.isExternal)
        {
            const auto *funcSyms = req.symbolTable.FindSymbolsPtr(sym.name);
            if (funcSyms)
            {
                for (const auto &fSym : *funcSyms)
                {
                    if (fSym.fileUri == sym.fileUri && fSym.startLine == sym.startLine &&
                        fSym.type == SymbolType::Function && !fSym.GetFunction().hasBody)
                    {
                        DebugDiag("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
                        break;
                    }
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateProperty(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (Rule_PropertyModifiers(sym, req, diagnostics))
            return;

        Rule_PropertyAccessors(sym, req, diagnostics);
    }
}
