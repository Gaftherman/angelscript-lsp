#include "analysis/SemanticAnalyzerInternal.h"

namespace angel_lsp::analysis
{
    bool SemanticAnalyzer::Rule_DuplicateTypeConflict(const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
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
                    DebugDiag("Rule_DuplicateTypeConflict", "as-err-name-conflict", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-name-conflict", sym.name, typeName));
                    hasConflict = true;
                }
            }
            if (hasConflict)
                return true;
        }

        return false;
    }

    bool SemanticAnalyzer::Rule_DuplicateVarCallableCollision(const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
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
            DebugDiag("Rule_DuplicateVarCallableCollision", "as-err-name-conflict", *varSym);
            diagnostics.push_back(CreateDiagnostic(*varSym, req, "as-err-name-conflict", varSym->name, "function"));
            return true;
        }
        if (hasEnum && hasVariable && varSym)
        {
            DebugDiag("Rule_DuplicateVarCallableCollision", "as-err-name-conflict", *varSym);
            diagnostics.push_back(CreateDiagnostic(*varSym, req, "as-err-name-conflict", varSym->name, "named type"));
            return true;
        }

        return false;
    }

    void SemanticAnalyzer::Rule_DuplicateSignature(const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const SymbolType firstType = symbols[0].type;
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
                DebugDiag("Rule_DuplicateSignature", "as-err-duplicate-symbol", *currentFileSymbols[i]);
                diagnostics.push_back(CreateDiagnostic(*currentFileSymbols[i], req, "as-err-duplicate-symbol", currentFileSymbols[i]->name));
            }
        }

        if (allSameType && (firstType == SymbolType::Function || firstType == SymbolType::Funcdef))
        {
            if (firstType == SymbolType::Funcdef)
            {
                std::vector<const Symbol *> currentFileSymbols;
                for (const auto &sym : symbols)
                {
                    if (sym.fileUri == req.fileUri)
                        currentFileSymbols.push_back(&sym);
                }

                for (size_t i = 1; i < currentFileSymbols.size(); ++i)
                {
                    DebugDiag("Rule_DuplicateSignature", "as-err-duplicate-symbol", *currentFileSymbols[i]);
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
                        DebugDiag("Rule_DuplicateSignature", "as-err-duplicate-symbol", symbols[i]);
                        diagnostics.push_back(CreateDiagnostic(symbols[i], req, "as-err-duplicate-symbol", symbols[i].name));
                        break;
                    }
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateDuplicates(const std::string &qualifiedName, const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (symbols.size() <= 1)
            return;

        if (symbols[0].type == SymbolType::Namespace)
            return;

        if (Rule_DuplicateTypeConflict(symbols, req, diagnostics))
            return;

        if (Rule_DuplicateVarCallableCollision(symbols, req, diagnostics))
            return;

        Rule_DuplicateSignature(symbols, req, diagnostics);
    }

    bool SemanticAnalyzer::Rule_InterfaceName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            DebugDiag("Rule_InterfaceName", "as-err-reserved-keyword-name", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return true;
        }

        return false;
    }

    void SemanticAnalyzer::Rule_InterfaceInheritance(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetInterface();

        if (sig.modifiers.isExternal)
        {
            DebugDiag("Rule_InterfaceInheritance", "as-err-external-not-found", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-external-not-found", sym.name));
        }

        for (const auto &ifaceName : sig.inheritedInterfaces)
        {
            if (ifaceName == sym.name || !req.symbolTable.HasSymbol(ifaceName))
            {
                DebugDiag("Rule_InterfaceInheritance", "as-err-base-not-found", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-base-not-found", ifaceName));
            }
        }
    }

    void SemanticAnalyzer::Rule_InterfaceMethods(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
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
                                DebugDiag("Rule_InterfaceMethods", "as-err-interface-private-method", ms);
                                diagnostics.push_back(CreateDiagnostic(ms, req, "as-err-interface-private-method", ms.name));
                            }
                            if (ms.name == sym.name || (!ms.name.empty() && ms.name[0] == '~'))
                            {
                                DebugDiag("Rule_InterfaceMethods", "as-err-interface-constructor", ms);
                                diagnostics.push_back(CreateDiagnostic(ms, req, "as-err-interface-constructor", ms.name));
                            }
                        }
                    }
                }
            });
    }

    void SemanticAnalyzer::ValidateInterface(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (Rule_InterfaceName(sym, req, diagnostics))
            return;

        Rule_InterfaceInheritance(sym, req, diagnostics);
        Rule_InterfaceMethods(sym, req, diagnostics);
    }

    bool SemanticAnalyzer::Rule_TypedefName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            DebugDiag("Rule_TypedefName", "as-err-reserved-keyword-name", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return true;
        }

        return false;
    }

    void SemanticAnalyzer::Rule_TypedefTypeResolution(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetTypedef();

        if (sig.typeKind == TypeKind::Void || sig.baseType == "void" || !IsPrimitiveTypeName(sig.baseType))
        {
            DebugDiag("Rule_TypedefTypeResolution", "as-err-typedef-non-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-typedef-non-primitive", sig.baseType));
        }
        else if (sig.typeKind == TypeKind::Unknown && !sig.baseType.empty())
        {
            if (!req.symbolTable.HasSymbol(sig.baseType))
            {
                DebugDiag("Rule_TypedefTypeResolution", "as-err-typedef-unresolved", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-typedef-unresolved", sig.baseType));
            }
        }
    }

    void SemanticAnalyzer::ValidateTypedef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (Rule_TypedefName(sym, req, diagnostics))
            return;

        Rule_TypedefTypeResolution(sym, req, diagnostics);
    }

    bool SemanticAnalyzer::Rule_FuncdefName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();

        const auto &sig = sym.GetFunction();
        if (sig.modifiers.isExternal)
        {
            DebugDiag("Rule_FuncdefName", "as-err-external-not-found", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-external-not-found", sym.name));
        }

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            DebugDiag("Rule_FuncdefName", "as-err-reserved-keyword-name", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return true;
        }

        return false;
    }

    void SemanticAnalyzer::Rule_FuncdefReturn(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();
        const auto &sig = sym.GetFunction();

        if (sig.returnHasPrimitiveHandle)
        {
            DebugDiag("Rule_FuncdefReturn", "as-err-handle-on-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }

        if (!sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" && !IsPrimitiveTypeName(sig.returnBaseTypeName) && sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName)
        {
            if (!req.symbolTable.HasSymbolAnywhere(sig.returnBaseTypeName))
            {
                DebugDiag("Rule_FuncdefReturn", "as-err-unresolved-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.returnBaseTypeName));
            }
        }
    }

    void SemanticAnalyzer::ValidateFuncdef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (Rule_FuncdefName(sym, req, diagnostics))
            return;

        const auto &sig = sym.GetFunction();
        Rule_FuncdefReturn(sym, req, diagnostics);
        ValidateFunctionParameters(sym, sig, req, diagnostics);
    }

    bool SemanticAnalyzer::Rule_EnumName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            DebugDiag("Rule_EnumName", "as-err-reserved-keyword-name", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return true;
        }

        return false;
    }

    void SemanticAnalyzer::Rule_EnumMembers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetEnum();

        if (!sig.hasBraces && sig.members.empty() && !sig.modifiers.isExternal)
        {
            DebugDiag("Rule_EnumMembers", "as-syntax-error", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (sig.modifiers.isExternal)
        {
            DebugDiag("Rule_EnumMembers", "as-err-external-not-found", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-external-not-found", sym.name));
        }

        ankerl::unordered_dense::set<std::string> seenEnumMembers;
        for (const auto &member : sig.members)
        {
            if (!seenEnumMembers.insert(member.name).second)
            {
                DebugDiag("Rule_EnumMembers", "as-err-name-conflict", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-name-conflict", member.name, "enum member"));
            }

            if (!member.value.empty())
            {
                std::string val = member.value;
                if (val == member.name)
                {
                    DebugDiag("Rule_EnumMembers", "as-err-unresolved-symbol", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-symbol", member.name));
                }
                else
                {
                    bool isStringLiteral = (member.valueNodeType == node_types::StringLiteral || (!val.empty() && (val.front() == '"' || val.front() == '\'')));
                    bool isLambda = (member.valueNodeType == node_types::LambdaExpression);
                    bool isBool = (member.valueNodeType == node_types::BooleanLiteral || val == "true" || val == "false");
                    bool isNull = (member.valueNodeType == node_types::NullLiteral || val == "null");
                    bool isTypeKeyword = (val == "int" || val == "float" || val == "double" || val == "void" || val == "auto" || val == "class" || val == "struct" || val == "enum");
                    bool isCallOrExpr = (member.valueNodeType == node_types::CallExpression);

                    if (isStringLiteral || isLambda || isBool || isNull || isTypeKeyword || isCallOrExpr)
                    {
                        DebugDiag("Rule_EnumMembers", "as-err-enum-invalid-initializer", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-enum-invalid-initializer", member.name));
                    }
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateEnum(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (Rule_EnumName(sym, req, diagnostics))
            return;

        Rule_EnumMembers(sym, req, diagnostics);
    }

    bool SemanticAnalyzer::Rule_NamespaceName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view arrayTN = req.GetArrayTypeName();
        std::string_view stringTN = req.GetStringTypeName();
        if (IsReservedKeyword(sym.name) || sym.name == arrayTN || sym.name == stringTN || IsPrimitiveTypeName(sym.name))
        {
            DebugDiag("Rule_NamespaceName", "as-err-reserved-keyword-name", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return true;
        }

        return false;
    }

    void SemanticAnalyzer::ValidateNamespace(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        Rule_NamespaceName(sym, req, diagnostics);
    }
}
