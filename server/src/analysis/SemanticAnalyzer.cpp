#include "analysis/SemanticAnalyzer.h"
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::analysis
{
    /** @brief Checks whether the given name is a reserved AngelScript keyword that
     *         cannot be used as a symbol name.
     *  Context-sensitive keywords (abstract, final, function, get, set, etc.) are intentionally
     *  excluded since they are valid identifiers per specification.
     *  @param name The symbol name to check.
     *  @return True if name is a reserved keyword. */
    static bool IsReservedKeyword(const std::string &name)
    {
        static const ankerl::unordered_dense::set<std::string> kReserved = {
            "and", "auto", "bool", "break", "case", "cast", "catch",
            "class", "const", "continue", "default", "do", "double",
            "else", "enum", "false", "float", "for", "foreach", "funcdef",
            "if", "import", "in", "inout", "int", "int8", "int16", "int32", "int64",
            "interface", "is", "mixin", "namespace", "not", "null",
            "or", "out", "private", "protected", "return", "switch",
            "true", "try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64",
            "using", "void", "while", "xor",
        };
        return kReserved.contains(name);
    }

    SemanticAnalyzer::SemanticAnalyzer(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
    }

    std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest &request) const
    {
        std::vector<Diagnostic> diagnostics;

        request.symbolTable.ForEachSymbol(
            [&](const std::string &qualifiedName, const std::vector<Symbol> &symbols)
            {
                ValidateDuplicates(qualifiedName, symbols, request, diagnostics);

                for (const Symbol &sym : symbols)
                {
                    if (sym.fileUri != request.fileUri)
                        continue;

                    switch (sym.type)
                    {
                    case SymbolType::Function:
                        ValidateFunction(sym, request, diagnostics);
                        break;
                    case SymbolType::Variable:
                        ValidateVariable(sym, request, diagnostics);
                        break;
                    case SymbolType::Property:
                        ValidateProperty(sym, request, diagnostics);
                        break;
                    case SymbolType::Class:
                        ValidateClass(sym, request, diagnostics);
                        break;
                    case SymbolType::Interface:
                        ValidateInterface(sym, request, diagnostics);
                        break;
                    case SymbolType::Typedef:
                        ValidateTypedef(sym, request, diagnostics);
                        break;
                    case SymbolType::Funcdef:
                        ValidateFuncdef(sym, request, diagnostics);
                        break;
                    case SymbolType::Namespace:
                        ValidateNamespace(sym, request, diagnostics);
                        break;
                    default:
                        break;
                    }
                }
            });

        return diagnostics;
    }

    void SemanticAnalyzer::ValidateDuplicates(const std::string &qualifiedName, const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (symbols.size() <= 1)
            return;

        const SymbolType firstType = symbols[0].type;

        if (firstType == SymbolType::Namespace)
            return;

        // 1. Cross-type name conflict detection (e.g. function/variable named after an existing class, interface, funcdef, or typedef)
        const Symbol *typeDefiningSymbol = nullptr;
        for (const auto &sym : symbols)
        {
            if (sym.type == SymbolType::Class     ||
                sym.type == SymbolType::Interface ||
                sym.type == SymbolType::Funcdef   ||
                sym.type == SymbolType::Typedef)
            {
                typeDefiningSymbol = &sym;
                break;
            }
        }

        if (typeDefiningSymbol != nullptr)
        {
            bool hasConflict = false;
            for (const auto &sym : symbols)
            {
                if (sym.fileUri != req.fileUri)
                    continue;
                if ((sym.type == SymbolType::Function || sym.type == SymbolType::Variable) &&
                    &sym != typeDefiningSymbol)
                {
                    const std::string typeName = SymbolTypeToString(typeDefiningSymbol->type);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-name-conflict", sym.name, typeName));
                    hasConflict = true;
                }
            }
            if (hasConflict)
                return;
        }

        // 2. Same-type duplicate validation
        bool allSameType = true;
        for (size_t i = 1; i < symbols.size(); ++i)
        {
            if (symbols[i].type != firstType)
            {
                allSameType = false;
                break;
            }
        }

        if (allSameType && firstType != SymbolType::Function && firstType != SymbolType::Funcdef)
        {
            std::vector<const Symbol *> currentFileSymbols;
            for (const auto &sym : symbols)
            {
                if (sym.fileUri == req.fileUri)
                    currentFileSymbols.push_back(&sym);
            }

            for (size_t i = 1; i < currentFileSymbols.size(); ++i)
            {
                diagnostics.push_back(CreateDiagnostic(*currentFileSymbols[i], req, "as-err-duplicate-symbol", currentFileSymbols[i]->name));
            }
            return;
        }

        if (allSameType && (firstType == SymbolType::Function || firstType == SymbolType::Funcdef))
        {
            if (firstType == SymbolType::Funcdef)
            {
                // Funcdefs cannot be overloaded by signature; any duplicate name in the same scope is an error.
                std::vector<const Symbol *> currentFileSymbols;
                for (const auto &sym : symbols)
                {
                    if (sym.fileUri == req.fileUri)
                        currentFileSymbols.push_back(&sym);
                }

                for (size_t i = 1; i < currentFileSymbols.size(); ++i)
                {
                    diagnostics.push_back(CreateDiagnostic(*currentFileSymbols[i], req, "as-err-duplicate-symbol", currentFileSymbols[i]->name));
                }
                return;
            }

            for (size_t i = 0; i < symbols.size(); ++i)
            {
                if (symbols[i].fileUri != req.fileUri)
                    continue;

                const auto &sigI = symbols[i].GetFunction();

                for (size_t j = 0; j < i; ++j)
                {
                    const auto &sigJ = symbols[j].GetFunction();

                    if (sigI.parameters.size() != sigJ.parameters.size())
                        continue;

                    bool paramsMatch = true;
                    for (size_t p = 0; p < sigI.parameters.size(); ++p)
                    {
                        const auto &pI = sigI.parameters[p];
                        const auto &pJ = sigJ.parameters[p];
                        if (pI.baseTypeName != pJ.baseTypeName ||
                            pI.isConst != pJ.isConst ||
                            pI.modifier != pJ.modifier ||
                            pI.isReference != pJ.isReference ||
                            pI.isHandle != pJ.isHandle)
                        {
                            paramsMatch = false;
                            break;
                        }
                    }

                    if (paramsMatch)
                    {
                        diagnostics.push_back(CreateDiagnostic(symbols[i], req, "as-err-duplicate-symbol", symbols[i].name));
                        break;
                    }
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateFunction(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (IsReservedKeyword(sym.name))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetFunction();

        if (!sig.hasBody && !sig.isInterfaceMethod && !sig.modifiers.isExternal && !sig.modifiers.isDelete)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-missing-body", sym.name));
        }

        if (sig.modifiers.isDelete && sig.hasBody)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-delete-with-body", sym.name));
        }

        if (sig.returnHasPrimitiveHandle)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.returnType.find("const") != std::string::npos)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-const-void-return"));
        }

        if (sig.returnTypeKind == TypeKind::Void && (sig.returnType.find('&') != std::string::npos || sig.modifiers.isReturnReference))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-reference"));
        }

        if (!sym.name.empty() && sym.name[0] == '~')
        {
            if (!sig.parameters.empty())
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-param", sym.name));
            }
            if (sig.modifiers.isDelete)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-delete", sym.name));
            }
        }

        // Operator overload arity checks
        if (sym.name.rfind("op", 0) == 0 && !sym.containerName.empty())
        {
            if (sym.name == "opAdd" || sym.name == "opSub" || sym.name == "opMul" || sym.name == "opDiv" ||
                sym.name == "opMod" || sym.name == "opPow" || sym.name == "opAnd" || sym.name == "opOr" ||
                sym.name == "opXor" || sym.name == "opShl" || sym.name == "opShr" || sym.name == "opUShr")
            {
                if (sig.parameters.size() != 1)
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-binary-operator-arity", sym.name));
                }
            }
            else if (sym.name == "opIndex")
            {
                if (sig.parameters.empty())
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-opindex-no-params", sym.name));
                }
            }
            else if (sym.name == "opEquals")
            {
                if (sig.returnType != "bool")
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-opequals-return-bool", sym.name));
                }
            }
            else if (sym.name == "opCmp")
            {
                if (sig.returnType != "int")
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-opcmp-return-int", sym.name));
                }
            }
        }

        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        if (!sym.containerName.empty())
        {
            const auto *containerSyms = req.symbolTable.FindSymbolsPtr(sym.containerName);
            if (containerSyms)
            {
                bool isClassContainer = false;
                for (const auto &cSym : *containerSyms)
                {
                    if (cSym.type == SymbolType::Class || cSym.type == SymbolType::Interface)
                    {
                        isClassContainer = true;
                        break;
                    }
                }

                if (!isClassContainer)
                {
                    if (sig.modifiers.isConst || sig.modifiers.isOverride || sig.modifiers.isFinal)
                    {
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-function-qualifiers", sym.name));
                    }
                }
                else
                {
                    // For member functions, verify override is only used when the class has a base class or interface
                    if (sig.modifiers.isOverride)
                    {
                        for (const auto &cSym : *containerSyms)
                        {
                            if (cSym.type == SymbolType::Class)
                            {
                                if (cSym.GetClass().bases.empty())
                                {
                                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-override-no-base", sym.name, sym.containerName));
                                }
                                else
                                {
                                    // Check if base classes or interfaces define this method
                                    bool foundInHierarchy = false;
                                    for (const auto &baseName : cSym.GetClass().bases)
                                    {
                                        std::string expectedQN = baseName + "::" + sym.name;
                                        if (req.symbolTable.HasSymbol(expectedQN))
                                        {
                                            foundInHierarchy = true;
                                            break;
                                        }
                                    }
                                    if (!foundInHierarchy)
                                    {
                                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-override-no-base", sym.name, sym.containerName));
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        else
        {
            if (sig.modifiers.isConst || sig.modifiers.isOverride || sig.modifiers.isFinal)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-function-qualifiers", sym.name));
            }
        }

        if (sig.returnTypeKind == TypeKind::Unknown && !sig.returnBaseTypeName.empty())
        {
            if (sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName &&
                !req.symbolTable.HasSymbol(sig.returnBaseTypeName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.returnBaseTypeName));
            }
        }

        ValidateFunctionParameters(sym, sig, req, diagnostics);
    }

    void SemanticAnalyzer::ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        ankerl::unordered_dense::set<std::string> seenParamNames;

        bool seenDefault = false;

        for (const auto &param : sig.parameters)
        {
            if (!param.defaultValue.empty())
            {
                seenDefault = true;
            }
            else if (seenDefault && sym.type != SymbolType::Funcdef)
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-default-param-order", param.name, sym.name));
            }

            if (param.typeKind == TypeKind::Void || param.baseTypeName == "void")
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-void-parameter", param.name, sym.name));
            }

            if (param.modifier == ParameterModifier::InOut && param.typeKind != TypeKind::Unknown &&
                param.typeKind != TypeKind::Auto)
            {
                // Primitives cannot be passed with &inout (only object types supporting handles/references can)
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-inout-on-primitive", param.baseTypeName));
            }

            if (!param.name.empty())
            {
                if (seenParamNames.contains(param.name))
                {
                    diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-duplicate-param", param.name, sym.name));
                }
                else
                {
                    seenParamNames.insert(param.name);
                }
            }

            if (param.hasPrimitiveHandle)
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-handle-on-primitive", param.baseTypeName));
            }

            if (param.modifier == ParameterModifier::Out && !param.defaultValue.empty() && param.typeKind != TypeKind::Unknown)
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-out-param-default", param.name));
            }

            if (param.modifier == ParameterModifier::Out && param.isConst)
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-const-out-param", param.name));
            }

            if (param.typeKind == TypeKind::Unknown && !param.baseTypeName.empty())
            {
                if (!req.symbolTable.HasSymbol(param.baseTypeName))
                {
                    diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-unresolved-type", param.baseTypeName));
                }
            }

            if (!param.name.empty())
            {
                const auto *globalSyms = req.symbolTable.FindSymbolsPtr(param.name);
                if (globalSyms)
                {
                    for (const auto &gSym : *globalSyms)
                    {
                        if (gSym.containerName.empty() && gSym.type == SymbolType::Variable)
                        {
                            diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-warn-shadow-global", param.name, DiagnosticSeverity::Warning));
                            break;
                        }
                    }
                }
            }

            if (!param.isHandle)
            {
                const auto *typeSyms = req.symbolTable.FindSymbolsPtr(param.baseTypeName);
                if (typeSyms)
                {
                    for (const auto &tSym : *typeSyms)
                    {
                        if (tSym.type == SymbolType::Funcdef)
                        {
                            diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-funcdef-not-handle", param.baseTypeName, param.baseTypeName));
                            break;
                        }
                    }
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateVariable(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (IsReservedKeyword(sym.name))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetVariable();

        if (!sym.containerName.empty())
        {
            const auto *containerSyms = req.symbolTable.FindSymbolsPtr(sym.containerName);
            if (containerSyms)
            {
                bool isClassContainer = false;
                for (const auto &cSym : *containerSyms)
                {
                    if (cSym.type == SymbolType::Class || cSym.type == SymbolType::Interface)
                    {
                        isClassContainer = true;
                        break;
                    }
                }

                if (!isClassContainer)
                {
                    if (sig.modifiers.access == AccessModifier::Private || sig.modifiers.access == AccessModifier::Protected)
                    {
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-variable-access-modifier", sym.name));
                    }
                }
            }
        }
        else
        {
            if (sig.modifiers.access == AccessModifier::Private || sig.modifiers.access == AccessModifier::Protected)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-variable-access-modifier", sym.name));
            }
        }

        if (sig.typeKind == TypeKind::Void)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-variable"));
        }

        if (sig.hasPrimitiveHandle)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
        }

        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        if (sig.modifiers.isHandle && sig.baseTypeName == stringTypeName)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
        }

        if (sig.typeKind == TypeKind::Unknown && sig.baseTypeName != "auto" && !sig.baseTypeName.empty())
        {
            if (sig.baseTypeName != stringTypeName && sig.baseTypeName != arrayTypeName &&
                !req.symbolTable.HasSymbol(sig.baseTypeName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.baseTypeName));
            }
        }

        if (!sig.templateName.empty() && sig.templateName != "int" && sig.templateName != "float" &&
            sig.templateName != "double" && sig.templateName != "uint" && sig.templateName != "bool" &&
            sig.templateName != stringTypeName && sig.templateName != "auto")
        {
            if (!req.symbolTable.HasSymbol(sig.templateName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.templateName));
            }
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
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-class-member-const", sym.name));
                        break;
                    }
                }
            }
        }

        if (!sig.modifiers.isHandle && req.symbolTable.HasSymbol(sig.baseTypeName))
        {
            auto typeSyms = req.symbolTable.FindSymbols(sig.baseTypeName);
            for (const auto &tSym : typeSyms)
            {
                if (tSym.type == SymbolType::Funcdef)
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-funcdef-not-handle", sig.baseTypeName, sig.baseTypeName));
                    break;
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateProperty(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetVariable();

        if (sig.typeKind == TypeKind::Void)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-variable"));
        }

        if (sig.hasPrimitiveHandle)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
        }

        if (sig.typeKind == TypeKind::Unknown && !sig.baseTypeName.empty())
        {
            if (!req.symbolTable.HasSymbol(sig.baseTypeName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.baseTypeName));
            }
        }

        // Property accessors in concrete classes or global/namespace scope must have an implementation body.
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

        if (!isInterfaceProperty && sig.modifiers.isProperty && !sym.GetVariable().modifiers.isExternal)
        {
            // If the symbol represents a property accessor declaration without a body in a non-interface context:
            const auto *funcSyms = req.symbolTable.FindSymbolsPtr(sym.name);
            if (funcSyms)
            {
                for (const auto &fSym : *funcSyms)
                {
                    if (fSym.fileUri == sym.fileUri && fSym.startLine == sym.startLine &&
                        fSym.type == SymbolType::Function && !fSym.GetFunction().hasBody)
                    {
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
                        break;
                    }
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (IsReservedKeyword(sym.name))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetClass();

        if (sig.modifiers.isMixin && sig.modifiers.isFinal)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-final", sym.name));
        }

        if (sig.modifiers.isMixin && sig.modifiers.isAbstract)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-abstract", sym.name));
        }

        if (sig.isTemplate && !req.predefinedFileExtension.empty() && req.fileUri != req.predefinedFileExtension)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-template-class-not-supported", sym.name));
        }

        uint32_t classBaseCount = 0;

        for (const auto &baseName : sig.bases)
        {
            if (!req.symbolTable.HasSymbol(baseName))
            {
                if (!sig.modifiers.isMixin)
                {
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
                        if (bSym.type == SymbolType::Class)
                        {
                            if (bSym.GetClass().modifiers.isMixin && !sig.modifiers.isMixin)
                            {
                                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-as-base", sym.name, baseName));
                            }

                            isBaseClass = true;
                            if (bSym.GetClass().modifiers.isFinal)
                            {
                                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-inherit-final", baseName));
                            }

                            // Check if derived class overrides any final method of this base class
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

                            // Step 1: Collect interface method signatures while holding ForEachSymbol's
                            // shared_lock. Intentionally do NOT call FindSymbolsPtr inside the lambda
                            // since both acquire m_mutex (shared_lock from the same thread = UB).
                            struct IfaceMethod
                            {
                                std::string name;
                                std::vector<ParameterInformation> params;
                            };
                            std::vector<IfaceMethod> ifaceMethods;

                            req.symbolTable.ForEachSymbol(
                                [&](const std::string & /*qName*/, const std::vector<Symbol> &symsInTable)
                                {
                                    for (const auto &ms : symsInTable)
                                    {
                                        if (ms.containerName == ifaceContainer &&
                                            ms.type == SymbolType::Function &&
                                            ms.GetFunction().isInterfaceMethod)
                                        {
                                            ifaceMethods.push_back({ms.name, ms.GetFunction().parameters});
                                        }
                                    }
                                });

                            // Step 2: After ForEachSymbol returns (lock released), look up each method
                            // in the class. FindSymbolsPtr is now safe to call.
                            for (const auto &ifaceMethod : ifaceMethods)
                            {
                                const std::string expectedQN = classContainer.empty()
                                    ? ifaceMethod.name
                                    : classContainer + "::" + ifaceMethod.name;

                                const auto *classMethodSyms = req.symbolTable.FindSymbolsPtr(expectedQN);

                                bool implemented = false;
                                if (classMethodSyms)
                                {
                                    for (const auto &cMethodSym : *classMethodSyms)
                                    {
                                        if (cMethodSym.type != SymbolType::Function ||
                                            cMethodSym.containerName != classContainer)
                                        {
                                            continue;
                                        }

                                        const auto &classParams = cMethodSym.GetFunction().parameters;
                                        if (classParams.size() != ifaceMethod.params.size())
                                            continue;

                                        bool paramsMatch = true;
                                        for (size_t p = 0; p < ifaceMethod.params.size(); ++p)
                                        {
                                            if (ifaceMethod.params[p].baseTypeName != classParams[p].baseTypeName)
                                            {
                                                paramsMatch = false;
                                                break;
                                            }
                                        }

                                        if (paramsMatch)
                                        {
                                            implemented = true;
                                            break;
                                        }
                                    }
                                }

                                if (!implemented)
                                {
                                    diagnostics.push_back(CreateDiagnostic(sym, req,
                                        "as-err-interface-impl-missing",
                                        sym.name, ifaceMethod.name, baseName));
                                }
                            }
                        }
                    }

                    if (isBaseClass)
                    {
                        classBaseCount++;
                        if (classBaseCount > 1)
                        {
                            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-multi-class-inherit", sym.name));
                        }
                    }
                }
            }
        }

        ankerl::unordered_dense::set<std::string> visited;
        if (CheckCircularInheritance(sym.name, req.symbolTable, visited))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-circular-inherit", sym.name));
        }
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

    void SemanticAnalyzer::ValidateInterface(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (IsReservedKeyword(sym.name))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetInterface();

        for (const auto &ifaceName : sig.inheritedInterfaces)
        {
            if (!req.symbolTable.HasSymbol(ifaceName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-base-not-found", ifaceName));
            }
        }
    }

    void SemanticAnalyzer::ValidateTypedef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (IsReservedKeyword(sym.name))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetTypedef();

        if (sig.typeKind == TypeKind::Unknown && !sig.baseType.empty())
        {
            if (!req.symbolTable.HasSymbol(sig.baseType))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-typedef-unresolved", sig.baseType));
            }
        }
    }

    void SemanticAnalyzer::ValidateFuncdef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (IsReservedKeyword(sym.name))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetFunction();

        if (sig.returnHasPrimitiveHandle)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }

        if (sig.returnTypeKind == TypeKind::Unknown && !sig.returnBaseTypeName.empty())
        {
            if (!req.symbolTable.HasSymbol(sig.returnBaseTypeName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.returnBaseTypeName));
            }
        }

        ValidateFunctionParameters(sym, sig, req, diagnostics);
    }

    void SemanticAnalyzer::ValidateNamespace(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (IsReservedKeyword(sym.name))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = sym.startLine;
        diag.range.start.character = sym.startCharacter;
        diag.range.end.line = sym.endLine;
        diag.range.end.character = sym.endCharacter;
        diag.severity = severity;
        diag.code = code;
        diag.source = "AngelScript";
        diag.fileUri = sym.fileUri;

        if (req.i18n)
        {
            diag.message = req.i18n->GetMessage(code);
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1);
            }
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1, arg2);
            }
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, const std::string &arg3, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1, arg2, arg3);
            }
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = param.startLine;
        diag.range.start.character = param.startCharacter;
        diag.range.end.line = param.endLine;
        diag.range.end.character = param.endCharacter;
        diag.severity = severity;
        diag.code = code;
        diag.source = "AngelScript";
        diag.fileUri = parentSym.fileUri;

        if (req.i18n)
        {
            diag.message = req.i18n->GetMessage(code);
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(param, parentSym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1);
            }
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(param, parentSym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1, arg2);
            }
        }

        return diag;
    }
}
