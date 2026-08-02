#include "analysis/SemanticAnalyzerInternal.h"

namespace angel_lsp::analysis
{
    bool SemanticAnalyzer::Rule_ClassName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            DebugDiag("Rule_ClassName", "as-err-reserved-keyword-name", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return true;
        }

        return false;
    }

    void SemanticAnalyzer::Rule_ClassModifiers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetClass();

        if (!sig.hasBraces && !sig.modifiers.isExternal)
        {
            DebugDiag("Rule_ClassModifiers", "as-syntax-error", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (sig.modifiers.isExternal && !sig.modifiers.isShared)
        {
            DebugDiag("Rule_ClassModifiers", "as-syntax-error", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (sig.modifiers.isExternal && sig.modifiers.isShared)
        {
            DebugDiag("Rule_ClassModifiers", "as-err-external-not-found", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-external-not-found", sym.name));
        }

        if (sig.modifiers.isMixin && sig.modifiers.isShared)
        {
            DebugDiag("Rule_ClassModifiers", "as-err-mixin-shared", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-shared", sym.name));
        }

        if (sig.modifiers.isMixin && sig.modifiers.isFinal)
        {
            DebugDiag("Rule_ClassModifiers", "as-err-mixin-final", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-final", sym.name));
        }

        if (sig.modifiers.isMixin && sig.modifiers.isAbstract)
        {
            DebugDiag("Rule_ClassModifiers", "as-err-mixin-abstract", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-abstract", sym.name));
        }

        if (sig.modifiers.isOverride || sig.modifiers.isExplicit)
        {
            DebugDiag("Rule_ClassModifiers", "as-syntax-error", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (sig.modifiers.isMixin)
        {
            if (!sig.bases.empty())
            {
                DebugDiag("Rule_ClassModifiers", "as-syntax-error", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }

            req.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &syms) {
                for (const auto &s : syms)
                {
                    if (s.containerName == sym.name &&
                        (s.type == SymbolType::Funcdef || s.type == SymbolType::Class || s.type == SymbolType::Enum || s.type == SymbolType::Typedef || s.type == SymbolType::Interface))
                    {
                        DebugDiag("Rule_ClassModifiers", "as-err-mixin-child-type", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-child-type", sym.name));
                        return;
                    }
                }
            });
        }

        if (sig.isTemplate && !req.predefinedFileExtension.empty() && req.fileUri != req.predefinedFileExtension && !req.fileUri.ends_with(req.predefinedFileExtension))
        {
            DebugDiag("Rule_ClassModifiers", "as-err-template-class-not-supported", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-template-class-not-supported", sym.name));
        }
    }

    void SemanticAnalyzer::Rule_ClassInheritance(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetClass();
        uint32_t classBaseCount = 0;

        for (const auto &baseName : sig.bases)
        {
            if (!req.symbolTable.HasSymbol(baseName))
            {
                if (!sig.modifiers.isMixin)
                {
                    DebugDiag("Rule_ClassInheritance", "as-err-base-not-found", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-base-not-found", baseName));
                }
            }
            else
            {
                const auto *baseSyms = req.symbolTable.FindSymbolsPtr(baseName);
                if (baseSyms)
                {
                    bool isBaseClass = false;

                    for (const auto &bSym : *baseSyms)
                    {
                        if (sig.modifiers.isShared)
                        {
                            bool isBaseShared = false;
                            if (bSym.type == SymbolType::Class)
                                isBaseShared = bSym.GetClass().modifiers.isShared;
                            else if (bSym.type == SymbolType::Interface)
                                isBaseShared = bSym.GetInterface().modifiers.isShared;
                            if (!isBaseShared)
                            {
                                DebugDiag("Rule_ClassInheritance", "as-err-base-not-found", sym);
                                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-base-not-found", baseName));
                            }
                        }

                        if (bSym.type == SymbolType::Class)
                        {
                            if (bSym.GetClass().modifiers.isMixin && !sig.modifiers.isMixin)
                            {
                                DebugDiag("Rule_ClassInheritance", "as-err-mixin-as-base", sym);
                                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-as-base", sym.name, baseName));
                            }

                            isBaseClass = true;
                            if (bSym.GetClass().modifiers.isFinal)
                            {
                                DebugDiag("Rule_ClassInheritance", "as-err-inherit-final", sym);
                                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-inherit-final", baseName));
                            }

                            std::string baseContainer = bSym.qualifiedName;
                            std::string derivedContainer = sym.qualifiedName;

                            req.symbolTable.ForEachSymbol(
                                [&](const std::string & /*qName*/, const std::vector<Symbol> &symsInTable)
                                {
                                    for (const auto &mSym : symsInTable)
                                    {
                                        if (mSym.containerName == baseContainer && mSym.type == SymbolType::Function &&
                                            mSym.GetFunction().modifiers.isFinal)
                                        {
                                            std::string derivedMethodQN = derivedContainer.empty() ? mSym.name : derivedContainer + "::" + mSym.name;
                                            if (req.symbolTable.HasSymbol(derivedMethodQN))
                                            {
                                                DebugDiag("Rule_ClassInheritance", "as-err-override-final-method", sym);
                                                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-override-final-method", mSym.name, baseName));
                                            }
                                        }
                                    }
                                });
                            break;
                        }
                        else if (bSym.type == SymbolType::Interface && !sig.modifiers.isMixin)
                        {
                            const std::string ifaceContainer = bSym.qualifiedName;
                            const std::string classContainer = sym.qualifiedName;

                            struct IfaceMethod
                            {
                                std::string name;
                                std::string returnType;
                                bool isConst = false;
                                std::vector<ParameterInformation> params;
                            };
                            struct IfaceProperty
                            {
                                std::string name;
                                std::string text;
                                bool hasSet = false;
                            };
                            std::vector<IfaceMethod> ifaceMethods;
                            std::vector<IfaceProperty> ifaceProperties;

                            req.symbolTable.ForEachSymbol(
                                [&](const std::string & /*qName*/, const std::vector<Symbol> &symsInTable)
                                {
                                    for (const auto &ms : symsInTable)
                                    {
                                        if (ms.containerName == baseName)
                                        {
                                            if (ms.type == SymbolType::Function)
                                            {
                                                ifaceMethods.push_back({ms.name, ms.GetFunction().returnType, ms.GetFunction().modifiers.isConst, ms.GetFunction().parameters});
                                            }
                                            else if (ms.type == SymbolType::Variable || ms.type == SymbolType::Property)
                                            {
                                                ifaceProperties.push_back({ms.name, ms.GetVariable().defaultValue, ms.GetVariable().hasSet});
                                            }
                                        }
                                    }
                                });

                            for (const auto &ifaceMethod : ifaceMethods)
                            {
                                bool implemented = false;
                                auto checkMatchInClass = [&](const std::string &container) -> bool {
                                    const std::string expectedQN = container.empty() ? ifaceMethod.name : container + "::" + ifaceMethod.name;
                                    const auto *classMethodSyms = req.symbolTable.FindSymbolsPtr(expectedQN);
                                    if (!classMethodSyms) return false;

                                    for (const auto &cMethodSym : *classMethodSyms)
                                    {
                                        if (cMethodSym.type != SymbolType::Function || cMethodSym.containerName != container)
                                            continue;

                                        const auto &cFunc = cMethodSym.GetFunction();
                                        if (cFunc.returnType != ifaceMethod.returnType || cFunc.modifiers.isConst != ifaceMethod.isConst)
                                            continue;

                                        const auto &classParams = cFunc.parameters;
                                        if (classParams.size() != ifaceMethod.params.size())
                                            continue;

                                        bool paramsMatch = true;
                                        for (size_t p = 0; p < ifaceMethod.params.size(); ++p)
                                        {
                                            const auto &ip = ifaceMethod.params[p];
                                            const auto &cp = classParams[p];
                                            if (ip.baseTypeName != cp.baseTypeName || ip.modifier != cp.modifier || ip.isConst != cp.isConst || ip.isReference != cp.isReference)
                                            {
                                                paramsMatch = false;
                                                break;
                                            }
                                        }

                                        if (paramsMatch) return true;
                                    }
                                    return false;
                                };

                                implemented = checkMatchInClass(classContainer);

                                if (!implemented)
                                {
                                    for (const auto &baseName : sig.bases)
                                    {
                                        const auto *baseSyms = req.symbolTable.FindSymbolsPtr(baseName);
                                        if (baseSyms)
                                        {
                                            bool isClassBase = false;
                                            for (const auto &b : *baseSyms)
                                            {
                                                if (b.type == SymbolType::Class)
                                                {
                                                    isClassBase = true;
                                                    break;
                                                }
                                            }
                                            if (isClassBase && checkMatchInClass(baseName))
                                            {
                                                implemented = true;
                                                break;
                                            }
                                        }
                                    }
                                }

                                if (!implemented)
                                {
                                    DebugDiag("Rule_ClassInheritance", "as-err-interface-impl-missing", sym);
                                    diagnostics.push_back(CreateDiagnostic(sym, req,
                                        "as-err-interface-impl-missing",
                                        sym.name, ifaceMethod.name, baseName));
                                }
                            }

                            for (const auto &ifaceProp : ifaceProperties)
                            {
                                bool propImplemented = false;
                                bool ifaceNeedsSet = ifaceProp.hasSet;
                                const std::string expectedPropQN = classContainer.empty() ? ifaceProp.name : classContainer + "::" + ifaceProp.name;
                                const auto *classPropSyms = req.symbolTable.FindSymbolsPtr(expectedPropQN);
                                if (classPropSyms)
                                {
                                    for (const auto &cPropSym : *classPropSyms)
                                    {
                                        if (cPropSym.containerName == classContainer)
                                        {
                                            bool classHasSet = cPropSym.GetVariable().hasSet;
                                            const std::string setFuncQN = classContainer.empty() ? "set_" + ifaceProp.name : classContainer + "::set_" + ifaceProp.name;
                                            if (req.symbolTable.HasSymbol(setFuncQN))
                                            {
                                                classHasSet = true;
                                            }
                                            if (!ifaceNeedsSet || classHasSet)
                                            {
                                                propImplemented = true;
                                                break;
                                            }
                                        }
                                    }
                                }

                                if (!propImplemented)
                                {
                                    DebugDiag("Rule_ClassInheritance", "as-err-interface-impl-missing", sym);
                                    diagnostics.push_back(CreateDiagnostic(sym, req,
                                        "as-err-interface-impl-missing",
                                        sym.name, ifaceProp.name, baseName));
                                }
                            }
                        }
                    }

                    if (isBaseClass)
                    {
                        classBaseCount++;
                        if (classBaseCount > 1)
                        {
                            DebugDiag("Rule_ClassInheritance", "as-err-multi-class-inherit", sym);
                            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-multi-class-inherit", sym.name));
                        }
                    }
                }
            }
        }

        ankerl::unordered_dense::set<std::string> visited;
        if (CheckCircularInheritance(sym.name, req.symbolTable, visited))
        {
            DebugDiag("Rule_ClassInheritance", "as-err-circular-inherit", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-circular-inherit", sym.name));
        }
    }

    void SemanticAnalyzer::ValidateClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (Rule_ClassName(sym, req, diagnostics))
            return;

        Rule_ClassModifiers(sym, req, diagnostics);
        Rule_ClassInheritance(sym, req, diagnostics);
    }

    bool SemanticAnalyzer::CheckCircularInheritance(const std::string &currentClass, const SymbolTable &table, ankerl::unordered_dense::set<std::string> &visited) const
    {
        if (visited.contains(currentClass))
        {
            return true;
        }

        visited.insert(currentClass);

        if (!table.HasSymbol(currentClass))
        {
            return false;
        }

        auto syms = table.FindSymbols(currentClass);
        for (const auto &sym : syms)
        {
            if (sym.type == SymbolType::Class)
            {
                const auto &sig = sym.GetClass();
                for (const auto &base : sig.bases)
                {
                    if (CheckCircularInheritance(base, table, visited))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    }
}
