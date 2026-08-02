#include "analysis/SemanticAnalyzerInternal.h"

namespace angel_lsp::analysis
{
    bool SemanticAnalyzer::Rule_VariableName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();

        if (sym.containerName.empty() && (sym.name == arrayTypeName || sym.name == stringTypeName))
        {
            DebugDiag("Rule_VariableName", "as-err-name-conflict", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-name-conflict", sym.name, "registered object type"));
            return true;
        }

        if (IsReservedKeyword(sym.name))
        {
            DebugDiag("Rule_VariableName", "as-err-reserved-keyword-name", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return true;
        }

        return false;
    }

    void SemanticAnalyzer::Rule_VariableModifiers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
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
                        DebugDiag("Rule_VariableModifiers", "as-err-class-member-const", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-class-member-const", sym.name));
                    }
                }
                else
                {
                    if (sig.modifiers.access == AccessModifier::Private || sig.modifiers.access == AccessModifier::Protected)
                    {
                        DebugDiag("Rule_VariableModifiers", "as-err-global-variable-access-modifier", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-variable-access-modifier", sym.name));
                    }
                }
            }
        }
        else
        {
            if (sig.modifiers.access == AccessModifier::Private || sig.modifiers.access == AccessModifier::Protected)
            {
                DebugDiag("Rule_VariableModifiers", "as-err-global-variable-access-modifier", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-variable-access-modifier", sym.name));
            }
        }

        if (sig.modifiers.isReturnReference)
        {
            DebugDiag("Rule_VariableModifiers", "as-err-standalone-reference", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-standalone-reference", sym.name));
        }
    }

    void SemanticAnalyzer::Rule_VariableTypeResolution(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();
        const auto &sig = sym.GetVariable();

        std::string rawBaseType = sig.baseTypeName;
        if (rawBaseType.rfind("::", 0) == 0)
        {
            rawBaseType = rawBaseType.substr(2);
        }

        if (rawBaseType.find("::") != std::string::npos)
        {
            std::string currentPrefix = "";
            size_t start = 0;
            size_t end = rawBaseType.find("::");
            bool missingNamespace = false;
            while (end != std::string::npos)
            {
                std::string part = rawBaseType.substr(start, end - start);
                if (!currentPrefix.empty())
                {
                    currentPrefix += "::";
                }
                currentPrefix += part;

                if (!req.symbolTable.HasSymbolAnywhere(currentPrefix) && !req.symbolTable.HasSymbol(currentPrefix))
                {
                    DebugDiag("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", part));
                    missingNamespace = true;
                    break;
                }

                start = end + 2;
                end = rawBaseType.find("::", start);
            }

            if (!missingNamespace)
            {
                if (!req.symbolTable.HasSymbolAnywhere(rawBaseType) && !req.symbolTable.HasSymbol(rawBaseType))
                {
                    std::string targetType = rawBaseType.substr(start);
                    DebugDiag("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", targetType));
                }
            }
        }

        if (sig.typeKind == TypeKind::Void)
        {
            DebugDiag("Rule_VariableTypeResolution", "as-err-void-variable", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-variable"));
        }

        if (sig.hasPrimitiveHandle)
        {
            DebugDiag("Rule_VariableTypeResolution", "as-err-handle-on-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
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
            DebugDiag("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", "auto"));
        }

        if (sig.modifiers.isHandle && sig.baseTypeName == stringTypeName)
        {
            DebugDiag("Rule_VariableTypeResolution", "as-err-handle-on-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
        }

        if (sig.templateArgumentTypes.size() > 1)
        {
            DebugDiag("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.templateName));
        }
        else
        {
            for (const auto &innerType : sig.templateArgumentTypes)
            {
                if (!innerType.empty() && !IsPrimitiveTypeName(innerType) && innerType != stringTypeName && innerType != arrayTypeName && !req.symbolTable.HasSymbolAnywhere(innerType))
                {
                    DebugDiag("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", innerType));
                }
            }
        }

        if (sig.templateArgumentTypes.empty() && (sig.baseTypeName == arrayTypeName || sig.isArray) && !sig.templateName.empty())
        {
            std::string tName = sig.templateName;
            if (!IsPrimitiveTypeName(tName) && tName != stringTypeName && tName != arrayTypeName && !req.symbolTable.HasSymbolAnywhere(tName))
            {
                DebugDiag("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", tName));
            }
        }

        if (sig.typeKind == TypeKind::Unknown && sig.baseTypeName != "auto" && !sig.baseTypeName.empty() && sig.baseTypeName.find("::") == std::string::npos)
        {
            if (sig.baseTypeName != stringTypeName && sig.baseTypeName != arrayTypeName &&
                !req.symbolTable.HasSymbolAnywhere(sig.baseTypeName))
            {
                DebugDiag("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
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
            DebugDiag("Rule_VariableTypeResolution", "as-err-array-invalid-template", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-array-invalid-template", sig.baseTypeName));
        }

        if (!sig.templateName.empty() && (sig.templateName == "int8" || !IsPrimitiveTypeName(sig.templateName)) &&
            sig.templateName != stringTypeName && sig.templateName != arrayTypeName && sig.templateName != "auto")
        {
            if (invalidTemplateArgs.count(sig.templateName))
            {
                DebugDiag("Rule_VariableTypeResolution", "as-err-array-invalid-template", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-array-invalid-template", sig.templateName));
            }
            else if (!req.symbolTable.HasSymbolAnywhere(sig.templateName))
            {
                DebugDiag("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.templateName));
            }
        }

        if (!sig.modifiers.isHandle && req.symbolTable.HasSymbol(sig.baseTypeName))
        {
            auto typeSyms = req.symbolTable.FindSymbols(sig.baseTypeName);
            for (const auto &tSym : typeSyms)
            {
                if (tSym.type == SymbolType::Funcdef)
                {
                    DebugDiag("Rule_VariableTypeResolution", "as-err-funcdef-not-handle", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-funcdef-not-handle", sig.baseTypeName, sig.baseTypeName));
                    break;
                }
            }
        }
    }

    void SemanticAnalyzer::Rule_VariableInitializer(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();
        const auto &sig = sym.GetVariable();

        static const ankerl::unordered_dense::set<std::string> invalidDefaultValues = {
            "class", "interface", "enum", "typedef", "funcdef", "namespace", "return"
        };
        if (invalidDefaultValues.contains(sig.defaultValue))
        {
            DebugDiag("Rule_VariableInitializer", "as-syntax-error", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (!sig.defaultValue.empty())
        {
            std::string trimmedDef = sig.defaultValue;
            size_t firstChar = trimmedDef.find_first_not_of(" \t\r\n");
            if (firstChar != std::string::npos)
                trimmedDef = trimmedDef.substr(firstChar);

            if (trimmedDef.rfind("{", 0) == 0)
            {
                if ((IsPrimitiveTypeName(sig.baseTypeName) || sig.baseTypeName == stringTypeName) && !sig.isArray && sig.baseTypeName != arrayTypeName)
                {
                    DebugDiag("Rule_VariableInitializer", "as-syntax-error", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
                }
                else if (sig.baseTypeName == arrayTypeName || sig.isArray || sig.templateName == arrayTypeName || sig.templateName == "array")
                {
                    if (sig.arrayDepth >= 2)
                    {
                        if (trimmedDef.find("{{") == std::string::npos)
                        {
                            DebugDiag("Rule_VariableInitializer", "as-syntax-error", sym);
                            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
                        }
                    }
                    else
                    {
                        std::string elemType = !sig.templateArgumentTypes.empty() ? sig.templateArgumentTypes[0] : sig.baseTypeName;
                        if ((sig.templateName == arrayTypeName || sig.templateName == "array" || sig.isArray) && (elemType == "int" || elemType == "bool"))
                        {
                            size_t openBrace = trimmedDef.find('{');
                            size_t closeBrace = trimmedDef.rfind('}');
                            if (openBrace != std::string::npos && closeBrace != std::string::npos && closeBrace > openBrace)
                            {
                                std::string inner = trimmedDef.substr(openBrace + 1, closeBrace - openBrace - 1);
                                std::stringstream ss(inner);
                                std::string item;
                                while (std::getline(ss, item, ','))
                                {
                                    item.erase(0, item.find_first_not_of(" \t\r\n"));
                                    size_t last = item.find_last_not_of(" \t\r\n");
                                    if (last != std::string::npos) item = item.substr(0, last + 1);

                                    if (item.empty()) continue;

                                    if (elemType == "int")
                                    {
                                        if (item.starts_with("\"") || item == "true" || item == "false" || item == "null" || item.starts_with("{"))
                                        {
                                            DebugDiag("Rule_VariableInitializer", "as-syntax-error", sym);
                                            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
                                            break;
                                        }
                                    }
                                    else if (elemType == "bool")
                                    {
                                        if (item == "1" || item == "0" || (item.find_first_not_of("0123456789") == std::string::npos && !item.empty()))
                                        {
                                            DebugDiag("Rule_VariableInitializer", "as-syntax-error", sym);
                                            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            std::string val = sig.defaultValue;
            bool isString = (!val.empty() && (val.front() == '"' || val.front() == '\''));
            bool isBool = (val == "true" || val == "false");
            bool isNumericType = (sig.typeKind == TypeKind::Int32 || sig.typeKind == TypeKind::Int16 ||
                                  sig.typeKind == TypeKind::Int64 || sig.typeKind == TypeKind::Float ||
                                  sig.typeKind == TypeKind::Double || sig.typeKind == TypeKind::UInt32 ||
                                  sig.typeKind == TypeKind::Int8 || sig.typeKind == TypeKind::UInt16);
            if (isNumericType && (isString || isBool))
            {
                DebugDiag("Rule_VariableInitializer", "as-err-enum-invalid-initializer", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-enum-invalid-initializer", sym.name));
            }
            bool isBoolType = (sig.typeKind == TypeKind::Bool || sig.baseTypeName == "bool");
            if (isBoolType && isString)
            {
                DebugDiag("Rule_VariableInitializer", "as-err-enum-invalid-initializer", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-enum-invalid-initializer", sym.name));
            }
            bool isArrayVar = sig.isArray || sig.baseTypeName == arrayTypeName;
            if (isArrayVar && !sig.modifiers.isHandle && val == "null")
            {
                DebugDiag("Rule_VariableInitializer", "as-err-handle-on-primitive", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
            }
            if (sig.baseTypeName == stringTypeName && (val == "null" || sig.hasNullInitializer))
            {
                DebugDiag("Rule_VariableInitializer", "as-err-handle-on-primitive", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
            }
        }
    }

    void SemanticAnalyzer::ValidateVariable(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (Rule_VariableName(sym, req, diagnostics))
            return;

        const auto &sig = sym.GetVariable();
        if (sig.isVirtualProperty)
        {
            ValidateProperty(sym, req, diagnostics);
            return;
        }

        Rule_VariableModifiers(sym, req, diagnostics);
        Rule_VariableTypeResolution(sym, req, diagnostics);
        Rule_VariableInitializer(sym, req, diagnostics);
    }
}
