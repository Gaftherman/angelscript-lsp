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

    static bool IsPrimitiveTypeName(const std::string &name)
    {
        static const ankerl::unordered_dense::set<std::string> kPrimitives = {
            "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64",
            "float", "double", "bool", "void"
        };
        return kPrimitives.contains(name);
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
                    case SymbolType::Enum:
                        ValidateEnum(sym, request, diagnostics);
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

        // 2. Function vs Variable and Enum vs Variable collisions
        bool hasFunction = false;
        bool hasEnum = false;
        bool hasVariable = false;
        const Symbol *varSym = nullptr;
        for (const auto &s : symbols)
        {
            if (s.fileUri != req.fileUri) continue;
            if (s.type == SymbolType::Function) hasFunction = true;
            if (s.type == SymbolType::Enum) hasEnum = true;
            if (s.type == SymbolType::Variable) { hasVariable = true; varSym = &s; }
        }
        if (hasFunction && hasVariable && varSym)
        {
            diagnostics.push_back(CreateDiagnostic(*varSym, req, "as-err-name-conflict", varSym->name, "function"));
            return;
        }
        if (hasEnum && hasVariable && varSym)
        {
            diagnostics.push_back(CreateDiagnostic(*varSym, req, "as-err-name-conflict", varSym->name, "named type"));
            return;
        }

        // 3. Same-type duplicate validation
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
                    if (sigI.modifiers.isConst != sigJ.modifiers.isConst)
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
        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        if (sym.containerName.empty() && (sym.name == arrayTypeName || sym.name == stringTypeName))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-name-conflict", sym.name, "registered object type"));
            return;
        }

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

        if ((sig.modifiers.isDelete || sig.modifiers.isExternal) && sig.hasBody)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-delete-with-body", sym.name));
        }

        if (sig.returnHasPrimitiveHandle || (sig.modifiers.isHandle && sig.returnBaseTypeName == stringTypeName))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }

        if (sig.returnBaseTypeName == "auto")
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", "auto"));
        }

        if (!sym.name.empty() && sym.name[0] == '~')
        {
            if (sig.modifiers.isShared || sig.modifiers.isExternal)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }

        if (sig.modifiers.isDelete)
        {
            bool isMixinMember = false;
            if (!sym.containerName.empty())
            {
                auto pOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
                if (pOpt && pOpt->type == SymbolType::Class && pOpt->GetClass().modifiers.isMixin)
                    isMixinMember = true;
            }
            bool isAutoGeneratable = isMixinMember || (sym.name == "opAssign" || sym.name == "opEquals" || sym.name == "opCmp" || (!sym.containerName.empty() && (sym.name == sym.containerName || sym.name == "~" + sym.containerName)));
            if (!isAutoGeneratable)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }

        if (sig.modifiers.isProperty)
        {
            if (!sym.name.starts_with("get_") && !sym.name.starts_with("set_"))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.returnIsConst)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-const-void-return"));
        }

        if (sig.modifiers.isExternal && !sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" && !IsPrimitiveTypeName(sig.returnBaseTypeName) && sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName && !req.symbolTable.HasSymbolAnywhere(sig.returnBaseTypeName))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.returnBaseTypeName));
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.modifiers.isReturnReference)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-reference"));
        }

        bool isInsideClass = false;
        if (!sym.containerName.empty())
        {
            auto parentOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && parentOpt->type == SymbolType::Class)
                isInsideClass = true;
        }

        if (sig.hasValueReturn && IsReservedKeyword(sig.returnExpression) && sig.returnExpression != "null" && sig.returnExpression != "true" && sig.returnExpression != "false")
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (sym.containerName.empty() && sig.hasValueReturn && sig.returnCallTargetName != stringTypeName && sig.returnCallTargetName != arrayTypeName)
        {
            req.symbolTable.ForEachSymbol([&](const std::string &qName, const std::vector<Symbol> &symsInTable) {
                size_t nsSep = qName.rfind("::");
                if (nsSep != std::string::npos)
                {
                    std::string unqualName = qName.substr(nsSep + 2);
                    if (!req.symbolTable.HasSymbol(unqualName))
                    {
                        for (const auto &s : symsInTable)
                        {
                            if (s.fileUri == req.fileUri && s.type == SymbolType::Class)
                            {
                                if (!s.name.empty() && sig.returnCallTargetName == unqualName)
                                {
                                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", unqualName));
                                }
                            }
                        }
                    }
                }
            });
        }



        if (!isInsideClass && (sig.modifiers.isOverride || sig.modifiers.isFinal || sig.modifiers.isExplicit || sig.modifiers.isDelete || sig.modifiers.access == AccessModifier::Protected || sig.modifiers.access == AccessModifier::Private))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-function-qualifiers", sym.name));
        }

        bool isCtor = (!sym.containerName.empty() && sym.name == sym.containerName);

        // Constructors and destructors cannot use class-level declaration modifiers (final, abstract).
        // 'final' and 'abstract' as declaration_modifiers only apply to class declarations.
        // e.g.: class C { final C() {} } -> invalid, causes assertion error in native compiler.
        // Note: 'class C { C() final {} }' is VALID (here 'final' is a func_attribute, not declaration_modifier).
        if (sig.modifiers.isDeclarationFinal || sig.modifiers.isDeclarationAbstract)
        {
            std::string errCode = isCtor ? "as-err-reserved-keyword-name" : "as-syntax-error";
            diagnostics.push_back(CreateDiagnostic(sym, req, errCode, sym.name));
        }

        if (sig.modifiers.isDelete && (sig.modifiers.isConst || (!isCtor && (sig.modifiers.isOverride || sig.modifiers.isFinal || sig.modifiers.isExplicit))))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-delete-with-other-qualifier", sym.name));
        }

        bool isReturnArray = sig.returnIsArray || sig.returnBaseTypeName == arrayTypeName;
        if (isReturnArray && !sig.modifiers.isHandle && !sig.returnHasPrimitiveHandle && sig.hasNullReturn)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }



        if (!sym.containerName.empty() && sig.returnType.empty() && sym.name != sym.containerName && sym.name[0] != '~')
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-missing-body", sym.name));
        }

        if (!isInsideClass && sig.modifiers.isReturnReference && IsPrimitiveTypeName(sig.returnBaseTypeName) && !sig.modifiers.isConst)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-invalid-reference-return", sig.returnBaseTypeName));
        }

        if (sig.returnCallTargetName == "super" && isInsideClass)
        {
            auto parentOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && parentOpt->type == SymbolType::Class && parentOpt->GetClass().bases.empty())
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-no-base-class", sym.name));
            }
        }


        if (sig.returnTypeKind != TypeKind::Void && sig.returnType != "void" && !isCtor && (!sym.name.empty() && sym.name[0] != '~'))
        {
            if (sig.hasEmptyReturn)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-return-type", sym.name));
            }
        }

        if (isCtor && !sig.returnBaseTypeName.empty())
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-return-type", sym.name));
        }

        if (isCtor || (!sym.name.empty() && sym.name[0] == '~'))
        {
            if (sig.hasValueReturn)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-return-type", sym.name));
            }
        }

        if (!sym.name.empty() && sym.name[0] == '~')
        {
            bool isUnnamedVoid = (sig.parameters.size() == 1 && (sig.parameters[0].baseTypeName == "void" || sig.parameters[0].typeKind == TypeKind::Void) && sig.parameters[0].name.empty());
            if (!sig.parameters.empty() && !isUnnamedVoid)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-param", sym.name));
            }
            if (!sig.returnType.empty())
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-return-type", sym.name));
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
                if (sig.parameters.size() > 2)
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-binary-operator-arity", sym.name));
                }
            }
            else if (sym.name == "opEquals")
            {
                if (sig.parameters.empty() || (sig.returnType != "bool" && sig.returnType != "int"))
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-opequals-return-bool", sym.name));
                }
            }
            else if (sym.name == "opCmp")
            {
                if (sig.returnType != "int" && sig.returnType != "bool")
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-opcmp-return-int", sym.name));
                }
            }
        }

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
                                bool isCtorDtor = (sym.name == cSym.name || (!sym.name.empty() && sym.name[0] == '~'));
                                if (cSym.GetClass().modifiers.isMixin || isCtorDtor)
                                {
                                    break;
                                }
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
        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        ankerl::unordered_dense::set<std::string> seenParamNames;

        bool seenDefault = false;

        for (const auto &param : sig.parameters)
        {
            if (sig.modifiers.isExternal && !param.baseTypeName.empty() && !IsPrimitiveTypeName(param.baseTypeName) && param.baseTypeName != stringTypeName && param.baseTypeName != arrayTypeName && !req.symbolTable.HasSymbolAnywhere(param.baseTypeName))
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-unresolved-type", param.baseTypeName));
            }

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
                bool isUnnamedVoid = (sig.parameters.size() == 1 && param.name.empty());
                if (!isUnnamedVoid && sym.type != SymbolType::Funcdef)
                {
                    diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-void-parameter", param.name, sym.name));
                }
            }

            if (param.baseTypeName == "auto")
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-unresolved-type", "auto"));
            }

            if (param.isHandle && param.baseTypeName == stringTypeName)
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-handle-on-primitive", param.baseTypeName));
            }

            if (param.modifier == ParameterModifier::InOut &&
                (param.isHandle || IsPrimitiveTypeName(param.baseTypeName) || param.baseTypeName == stringTypeName || param.typeKind == TypeKind::String || param.typeKind == TypeKind::Int32 || param.typeKind == TypeKind::Float || param.typeKind == TypeKind::Bool))
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-inout-on-primitive", param.baseTypeName));
            }

            if (param.hasDoubleReference)
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-double-reference", param.baseTypeName));
            }

            bool isStandaloneRef = param.isStandaloneRef;
            if (isStandaloneRef && (IsPrimitiveTypeName(param.baseTypeName) || param.typeKind == TypeKind::Int32 || param.typeKind == TypeKind::Float || param.typeKind == TypeKind::Bool || param.typeKind == TypeKind::Double || param.typeKind == TypeKind::UInt32 || param.baseTypeName == stringTypeName))
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-standalone-reference", param.name));
            }
            else if (isStandaloneRef && !param.defaultValue.empty())
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-invalid-reference-return", param.baseTypeName));
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

            if (param.typeKind == TypeKind::Unknown && !param.baseTypeName.empty())
            {
                if (param.baseTypeName != stringTypeName && param.baseTypeName != arrayTypeName && !req.symbolTable.HasSymbol(param.baseTypeName) && !req.symbolTable.HasSymbolAnywhere(param.baseTypeName))
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
        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        if (sym.containerName.empty() && (sym.name == arrayTypeName || sym.name == stringTypeName))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-name-conflict", sym.name, "registered object type"));
            return;
        }

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

                if (isClassContainer)
                {
                    if (sig.modifiers.isConst)
                    {
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-class-member-const", sym.name));
                    }
                }
                else
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

        static const ankerl::unordered_dense::set<std::string> invalidDefaultValues = {
            "class", "interface", "enum", "typedef", "funcdef", "namespace", "return"
        };
        if (invalidDefaultValues.contains(sig.defaultValue))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (sig.isVirtualProperty)
        {
            ValidateProperty(sym, req, diagnostics);
            return;
        }

        if (sig.typeKind == TypeKind::Void)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-variable"));
        }

        if (sig.modifiers.isReturnReference)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-standalone-reference", sym.name));
        }

        if (sig.hasPrimitiveHandle)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
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

        if (!sig.defaultValue.empty())
        {
            std::string val = sig.defaultValue;
            bool isString = (!val.empty() && (val.front() == '"' || val.front() == '\''));
            bool isBool = (val == "true" || val == "false");
            bool isNumericType = (sig.typeKind == TypeKind::Int32 || sig.typeKind == TypeKind::Int16 ||
                                  sig.typeKind == TypeKind::Int64 || sig.typeKind == TypeKind::Float ||
                                  sig.typeKind == TypeKind::Double || sig.typeKind == TypeKind::UInt32 ||
                                  sig.typeKind == TypeKind::Int8 || sig.typeKind == TypeKind::UInt16);
            if (isNumericType && (isString || isBool))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-enum-invalid-initializer", sym.name));
            }
            bool isBoolType = (sig.typeKind == TypeKind::Bool || sig.baseTypeName == "bool");
            if (isBoolType && isString)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-enum-invalid-initializer", sym.name));
            }
            bool isArrayVar = sig.isArray || sig.baseTypeName == arrayTypeName;
            if (isArrayVar && !sig.modifiers.isHandle && val == "null")
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
            }
            if (sig.baseTypeName == stringTypeName && (val == "null" || sig.hasNullInitializer))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
            }
        }

        bool isInsideClass = false;
        if (!sym.containerName.empty())
        {
            auto parentOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && parentOpt->type == SymbolType::Class)
                isInsideClass = true;
        }

        if (sig.baseTypeName == "auto" && (isInsideClass || sig.defaultValue == "null"))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", "auto"));
        }

        if (sig.modifiers.isHandle && sig.baseTypeName == stringTypeName)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
        }

        if (sig.templateArgumentTypes.size() > 1)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.templateName));
        }
        else
        {
            for (const auto &innerType : sig.templateArgumentTypes)
            {
                if (!innerType.empty() && !IsPrimitiveTypeName(innerType) && innerType != stringTypeName && innerType != arrayTypeName && !req.symbolTable.HasSymbolAnywhere(innerType))
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", innerType));
                }
            }
        }

        if (sig.templateArgumentTypes.empty() && (sig.baseTypeName == arrayTypeName || sig.isArray) && !sig.templateName.empty())
        {
            std::string tName = sig.templateName;
            if (!IsPrimitiveTypeName(tName) && tName != stringTypeName && tName != arrayTypeName && !req.symbolTable.HasSymbolAnywhere(tName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", tName));
            }
        }

        if (sig.typeKind == TypeKind::Unknown && sig.baseTypeName != "auto" && !sig.baseTypeName.empty())
        {
            if (sig.baseTypeName != stringTypeName && sig.baseTypeName != arrayTypeName &&
                !req.symbolTable.HasSymbolAnywhere(sig.baseTypeName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.baseTypeName));
            }
        }

        static const ankerl::unordered_dense::set<std::string_view> invalidTemplateArgs = {
            "void", "auto", "class", "struct", "enum", "funcdef",
            "interface", "namespace", "using", "import", "export",
            "external", "shared", "final", "abstract", "true", "false", "null"
        };

        if (sig.isArray && invalidTemplateArgs.count(sig.baseTypeName))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-array-invalid-template", sig.baseTypeName));
        }

        if (!sig.templateName.empty() && sig.templateName != "int" && sig.templateName != "float" &&
            sig.templateName != "double" && sig.templateName != "uint" && sig.templateName != "bool" &&
            sig.templateName != stringTypeName && sig.templateName != arrayTypeName && sig.templateName != "auto")
        {
            if (invalidTemplateArgs.count(sig.templateName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-array-invalid-template", sig.templateName));
            }
            else if (!req.symbolTable.HasSymbolAnywhere(sig.templateName))
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

        if (sym.containerName.empty() && (sig.modifiers.isProperty || sig.isVirtualProperty || sig.hasGet || sig.hasSet))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-function-qualifiers", sym.name));
        }

        if (isInterfaceProperty && (sig.hasBodyGet || sig.hasBodySet))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (!isInterfaceProperty && ((sig.hasGet && !sig.hasBodyGet) || (sig.hasSet && !sig.hasBodySet)))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (sig.hasDuplicateGet || sig.hasDuplicateSet)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (isInterfaceProperty && (sig.isGetFinal || sig.isGetOverride || sig.isSetFinal || sig.isSetOverride))
        {
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
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-override-no-base", sym.name, sym.containerName));
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
        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetClass();

        if (sig.modifiers.isExternal && !sig.modifiers.isShared)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (sig.modifiers.isExternal && sig.modifiers.isShared)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-external-not-found", sym.name));
        }

        if (sig.modifiers.isMixin && sig.modifiers.isShared)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-shared", sym.name));
        }

        if (sig.modifiers.isMixin && sig.modifiers.isFinal)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-final", sym.name));
        }

        if (sig.modifiers.isMixin && sig.modifiers.isAbstract)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-abstract", sym.name));
        }

        if (sig.modifiers.isMixin)
        {
            req.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &syms) {
                for (const auto &s : syms)
                {
                    if (s.containerName == sym.name &&
                        (s.type == SymbolType::Funcdef || s.type == SymbolType::Class || s.type == SymbolType::Enum || s.type == SymbolType::Typedef || s.type == SymbolType::Interface))
                    {
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-child-type", sym.name));
                        return;
                    }
                }
            });
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
                        if (sig.modifiers.isShared)
                        {
                            bool isBaseShared = false;
                            if (bSym.type == SymbolType::Class)
                                isBaseShared = bSym.GetClass().modifiers.isShared;
                            else if (bSym.type == SymbolType::Interface)
                                isBaseShared = bSym.GetInterface().modifiers.isShared;
                            if (!isBaseShared)
                            {
                                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-base-not-found", baseName));
                            }
                        }

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
                                    // Check base class hierarchy (excluding interfaces)
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
        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetInterface();

        if (sig.modifiers.isExternal)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-external-not-found", sym.name));
        }

        for (const auto &ifaceName : sig.inheritedInterfaces)
        {
            if (ifaceName == sym.name || !req.symbolTable.HasSymbol(ifaceName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-base-not-found", ifaceName));
            }
        }

        const std::string ifaceContainer = sym.qualifiedName;
        req.symbolTable.ForEachSymbol(
            [&](const std::string & /*qName*/, const std::vector<Symbol> &symsInTable)
            {
                for (const auto &ms : symsInTable)
                {
                    if (ms.containerName == ifaceContainer && ms.fileUri == req.fileUri)
                    {
                        if (ms.type == SymbolType::Function)
                        {
                            const auto &fnSig = ms.GetFunction();
                            if (fnSig.modifiers.access == AccessModifier::Private || fnSig.modifiers.access == AccessModifier::Protected)
                            {
                                diagnostics.push_back(CreateDiagnostic(ms, req, "as-err-interface-private-method", ms.name));
                            }
                            if (ms.name == sym.name || (!ms.name.empty() && ms.name[0] == '~'))
                            {
                                diagnostics.push_back(CreateDiagnostic(ms, req, "as-err-interface-constructor", ms.name));
                            }
                        }
                    }
                }
            });
    }

    void SemanticAnalyzer::ValidateTypedef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetTypedef();

        if (sig.typeKind == TypeKind::Void || sig.baseType == "void" || !IsPrimitiveTypeName(sig.baseType))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-typedef-non-primitive", sig.baseType));
        }
        else if (sig.typeKind == TypeKind::Unknown && !sig.baseType.empty())
        {
            if (!req.symbolTable.HasSymbol(sig.baseType))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-typedef-unresolved", sig.baseType));
            }
        }
    }

    void SemanticAnalyzer::ValidateFuncdef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        const auto &sig = sym.GetFunction();
        if (sig.modifiers.isExternal)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-external-not-found", sym.name));
        }

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        if (sig.returnHasPrimitiveHandle)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }

        if (!sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" && !IsPrimitiveTypeName(sig.returnBaseTypeName) && sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName)
        {
            if (!req.symbolTable.HasSymbolAnywhere(sig.returnBaseTypeName))
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.returnBaseTypeName));
            }
        }

        ValidateFunctionParameters(sym, sig, req, diagnostics);
    }

    void SemanticAnalyzer::ValidateEnum(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = (req.typeConfig && !req.typeConfig->stringTypeName.empty()) ? req.typeConfig->stringTypeName : "string";
        std::string_view arrayTypeName = (req.typeConfig && !req.typeConfig->arrayTypeName.empty()) ? req.typeConfig->arrayTypeName : "array";

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return;
        }

        const auto &sig = sym.GetEnum();

        if (!sig.hasBraces && sig.members.empty() && !sig.modifiers.isExternal)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (sig.modifiers.isExternal)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-external-not-found", sym.name));
        }

        ankerl::unordered_dense::set<std::string> seenEnumMembers;
        for (const auto &member : sig.members)
        {
            if (!seenEnumMembers.insert(member.name).second)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-name-conflict", member.name, "enum member"));
            }

            if (!member.value.empty())
            {
                std::string val = member.value;
                if (val == member.name)
                {
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-symbol", member.name));
                }
                else
                {
                    bool isStringLiteral = (member.valueNodeType == "string_literal" || (!val.empty() && (val.front() == '"' || val.front() == '\'')));
                    bool isLambda = (member.valueNodeType == "lambda_expression");
                    bool isBool = (member.valueNodeType == "boolean_literal" || val == "true" || val == "false");
                    bool isNull = (member.valueNodeType == "null_literal" || val == "null");
                    bool isTypeKeyword = (val == "int" || val == "float" || val == "double" || val == "void" || val == "auto" || val == "class" || val == "struct" || val == "enum");
                    bool isCallOrExpr = (member.valueNodeType == "call_expression");

                    if (isStringLiteral || isLambda || isBool || isNull || isTypeKeyword || isCallOrExpr)
                    {
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-enum-invalid-initializer", member.name));
                    }
                }
            }
        }
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
