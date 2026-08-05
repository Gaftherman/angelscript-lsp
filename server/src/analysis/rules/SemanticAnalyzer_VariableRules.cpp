#include "analysis/rules/VariableRules.h"
#include "analysis/rules/PropertyRules.h"
#include "analysis/SemanticHelpers.h"
#include <sstream>

namespace angel_lsp::analysis::rules
{
    static bool Rule_VariableName(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        if (sym.containerName.empty() && (sym.name == arrayTypeName || sym.name == stringTypeName))
        {
            ctx.LogRule("Rule_VariableName", "as-err-name-conflict", sym);
            ctx.Emit(sym, "as-err-name-conflict", sym.name, "registered object type");
            return true;
        }

        if (IsReservedKeyword(sym.name))
        {
            ctx.LogRule("Rule_VariableName", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return true;
        }

        return false;
    }

    static void Rule_VariableModifiers(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetVariable();

        if (!sym.containerName.empty())
        {
            auto containerSyms = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
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
                        ctx.LogRule("Rule_VariableModifiers", "as-err-class-member-const", sym);
                        ctx.Emit(sym, "as-err-class-member-const", sym.name);
                    }
                }
                else
                {
                    if (sig.modifiers.access == AccessModifier::Private || sig.modifiers.access == AccessModifier::Protected)
                    {
                        ctx.LogRule("Rule_VariableModifiers", "as-err-global-variable-access-modifier", sym);
                        ctx.Emit(sym, "as-err-global-variable-access-modifier", sym.name);
                    }
                }
            }
        }
        else
        {
            if (sig.modifiers.access == AccessModifier::Private || sig.modifiers.access == AccessModifier::Protected)
            {
                ctx.LogRule("Rule_VariableModifiers", "as-err-global-variable-access-modifier", sym);
                ctx.Emit(sym, "as-err-global-variable-access-modifier", sym.name);
            }
        }

        if (sig.modifiers.isReturnReference)
        {
            ctx.LogRule("Rule_VariableModifiers", "as-err-standalone-reference", sym);
            ctx.Emit(sym, "as-err-standalone-reference", sym.name);
        }
    }

    static void Rule_VariableTypeResolution(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();
        const auto &sig = sym.GetVariable();

        std::string rawBaseType = sig.baseTypeName;
        if (rawBaseType.rfind("::", 0) == 0)
        {
            rawBaseType = rawBaseType.substr(2);
        }

        if (IsMixinClass(rawBaseType, ctx.request.symbolTable))
        {
            ctx.LogRule("Rule_VariableTypeResolution", "as-err-mixin-not-a-type", sym);
            ctx.Emit(sym, "as-err-mixin-not-a-type", sym.name);
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

                if (!ctx.request.symbolTable.HasSymbolAnywhere(currentPrefix) && !ctx.request.symbolTable.HasSymbol(currentPrefix))
                {
                    ctx.LogRule("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                    ctx.Emit(sym, "as-err-unresolved-type", part);
                    missingNamespace = true;
                    break;
                }

                start = end + 2;
                end = rawBaseType.find("::", start);
            }

            if (!missingNamespace)
            {
                if (!ctx.request.symbolTable.HasSymbolAnywhere(rawBaseType) && !ctx.request.symbolTable.HasSymbol(rawBaseType))
                {
                    std::string targetType = rawBaseType.substr(start);
                    ctx.LogRule("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                    ctx.Emit(sym, "as-err-unresolved-type", targetType);
                }
            }
        }

        if (sig.typeKind == TypeKind::Void)
        {
            ctx.LogRule("Rule_VariableTypeResolution", "as-err-void-variable", sym);
            ctx.Emit(sym, "as-err-void-variable");
        }

        if (sig.hasPrimitiveHandle)
        {
            ctx.LogRule("Rule_VariableTypeResolution", "as-err-handle-on-primitive", sym);
            ctx.Emit(sym, "as-err-handle-on-primitive", sig.baseTypeName);
        }

        bool isInsideClass = false;
        if (!sym.containerName.empty())
        {
            auto parentOpt = ctx.request.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && parentOpt->type == SymbolType::Class)
                isInsideClass = true;
        }

        if (sig.baseTypeName == "auto" && (isInsideClass || sig.defaultValue == "null" || sig.defaultValue.starts_with("function")))
        {
            ctx.LogRule("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
            ctx.Emit(sym, "as-err-unresolved-type", "auto");
        }

        if (sig.modifiers.isHandle && sig.baseTypeName == stringTypeName)
        {
            ctx.LogRule("Rule_VariableTypeResolution", "as-err-handle-on-primitive", sym);
            ctx.Emit(sym, "as-err-handle-on-primitive", sig.baseTypeName);
        }

        if (sig.templateArgumentTypes.size() > 1)
        {
            ctx.LogRule("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
            ctx.Emit(sym, "as-err-unresolved-type", sig.templateName);
        }
        else
        {
            for (const auto &innerType : sig.templateArgumentTypes)
            {
                if (IsMixinClass(innerType, ctx.request.symbolTable))
                {
                    ctx.LogRule("Rule_VariableTypeResolution", "as-err-mixin-not-a-type", sym);
                    ctx.Emit(sym, "as-err-mixin-not-a-type", innerType);
                }
                else if (!innerType.empty() && !IsKnownType(innerType, ctx))
                {
                    ctx.LogRule("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                    ctx.Emit(sym, "as-err-unresolved-type", innerType);
                }
            }
        }

        if (sig.templateArgumentTypes.empty() && (sig.baseTypeName == arrayTypeName || sig.isArray) && !sig.templateName.empty())
        {
            std::string tName = sig.templateName;
            if (IsMixinClass(tName, ctx.request.symbolTable))
            {
                ctx.LogRule("Rule_VariableTypeResolution", "as-err-mixin-not-a-type", sym);
                ctx.Emit(sym, "as-err-mixin-not-a-type", tName);
            }
            else if (!IsKnownType(tName, ctx))
            {
                ctx.LogRule("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                ctx.Emit(sym, "as-err-unresolved-type", tName);
            }
        }

        if (sig.typeKind == TypeKind::Unknown && sig.baseTypeName != "auto" && !sig.baseTypeName.empty() && sig.baseTypeName.find("::") == std::string::npos)
        {
            if (!IsKnownType(sig.baseTypeName, ctx))
            {
                ctx.LogRule("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                ctx.Emit(sym, "as-err-unresolved-type", sig.baseTypeName);
            }
        }

        auto isInvalidTemplateArg = [](const std::string &typeName) -> bool {
            if (typeName == "void" || typeName == "auto" || typeName == "null") return true;
            return IsReservedKeyword(typeName) && !IsPrimitiveTypeName(typeName);
        };

        if (sig.isArray && isInvalidTemplateArg(sig.baseTypeName))
        {
            ctx.LogRule("Rule_VariableTypeResolution", "as-err-array-invalid-template", sym);
            ctx.Emit(sym, "as-err-array-invalid-template", sig.baseTypeName);
        }

        if (!sig.templateName.empty() && sig.templateName != stringTypeName && sig.templateName != arrayTypeName && sig.templateName != "auto")
        {
            if (isInvalidTemplateArg(sig.templateName))
            {
                ctx.LogRule("Rule_VariableTypeResolution", "as-err-array-invalid-template", sym);
                ctx.Emit(sym, "as-err-array-invalid-template", sig.templateName);
            }
            else if (!ctx.request.symbolTable.HasSymbolAnywhere(sig.templateName))
            {
                ctx.LogRule("Rule_VariableTypeResolution", "as-err-unresolved-type", sym);
                ctx.Emit(sym, "as-err-unresolved-type", sig.templateName);
            }
        }

        if (!sig.modifiers.isHandle && ctx.request.symbolTable.HasSymbol(sig.baseTypeName))
        {
            auto typeSyms = ctx.request.symbolTable.FindSymbols(sig.baseTypeName);
            for (const auto &tSym : typeSyms)
            {
                if (tSym.type == SymbolType::Funcdef)
                {
                    ctx.LogRule("Rule_VariableTypeResolution", "as-err-funcdef-not-handle", sym);
                    ctx.Emit(sym, "as-err-funcdef-not-handle", sig.baseTypeName, sig.baseTypeName);
                    break;
                }
            }
        }
    }

    static void Rule_VariableInitializer(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();
        const auto &sig = sym.GetVariable();

        static const ankerl::unordered_dense::set<std::string> invalidDefaultValues = {
            "class", "interface", "enum", "typedef", "funcdef", "namespace", "return"
        };
        if (invalidDefaultValues.contains(sig.defaultValue))
        {
            ctx.LogRule("Rule_VariableInitializer", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
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
                    ctx.LogRule("Rule_VariableInitializer", "as-syntax-error", sym);
                    ctx.Emit(sym, "as-syntax-error");
                }
                else if (sig.baseTypeName == arrayTypeName || sig.isArray || sig.templateName == arrayTypeName || sig.templateName == "array")
                {
                    if (sig.arrayDepth >= 2)
                    {
                        if (trimmedDef.find("{{") == std::string::npos)
                        {
                            ctx.LogRule("Rule_VariableInitializer", "as-syntax-error", sym);
                            ctx.Emit(sym, "as-syntax-error");
                        }
                    }
                    else
                    {
                        std::string elemType = !sig.templateArgumentTypes.empty() ? sig.templateArgumentTypes[0] : sig.baseTypeName;
                        bool isNumericElem = (IsPrimitiveTypeName(elemType) && elemType != "bool" && elemType != "void");
                        bool isBoolElem = (elemType == "bool");
                        if ((sig.templateName == arrayTypeName || sig.templateName == "array" || sig.isArray) && (isNumericElem || isBoolElem))
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

                                    if (isNumericElem)
                                    {
                                        InitializerItemKind itemKind = ClassifyInitializerItem(item);
                                        if (itemKind != InitializerItemKind::NumericOrExpression)
                                        {
                                            ctx.LogRule("Rule_VariableInitializer", "as-syntax-error", sym);
                                            ctx.Emit(sym, "as-syntax-error");
                                            break;
                                        }
                                    }
                                    else if (isBoolElem)
                                    {
                                        if (item == "1" || item == "0" || (item.find_first_not_of("0123456789") == std::string::npos && !item.empty()))
                                        {
                                            ctx.LogRule("Rule_VariableInitializer", "as-syntax-error", sym);
                                            ctx.Emit(sym, "as-syntax-error");
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
                ctx.LogRule("Rule_VariableInitializer", "as-err-enum-invalid-initializer", sym);
                ctx.Emit(sym, "as-err-enum-invalid-initializer", sym.name);
            }
            bool isBoolType = (sig.typeKind == TypeKind::Bool || sig.baseTypeName == "bool");
            if (isBoolType && isString)
            {
                ctx.LogRule("Rule_VariableInitializer", "as-err-enum-invalid-initializer", sym);
                ctx.Emit(sym, "as-err-enum-invalid-initializer", sym.name);
            }
            bool isArrayVar = sig.isArray || sig.baseTypeName == arrayTypeName;
            if (isArrayVar && !sig.modifiers.isHandle && val == "null")
            {
                ctx.LogRule("Rule_VariableInitializer", "as-err-handle-on-primitive", sym);
                ctx.Emit(sym, "as-err-handle-on-primitive", sig.baseTypeName);
            }
            if (sig.baseTypeName == stringTypeName && (val == "null" || sig.hasNullInitializer))
            {
                ctx.LogRule("Rule_VariableInitializer", "as-err-handle-on-primitive", sym);
                ctx.Emit(sym, "as-err-handle-on-primitive", sig.baseTypeName);
            }

            std::string rhsName = val;
            while (!rhsName.empty() && isspace(static_cast<unsigned char>(rhsName.back()))) rhsName.pop_back();
            auto rhsSymOpt = ctx.request.symbolTable.FindFirstSymbol(rhsName);
            if (rhsSymOpt && rhsSymOpt->type == SymbolType::Variable)
            {
                std::string rhsType = rhsSymOpt->GetVariable().baseTypeName;
                std::string lhsType = sig.baseTypeName;
                if (IsPrimitiveTypeName(lhsType) && IsPrimitiveTypeName(rhsType))
                {
                    if ((lhsType == "bool" && rhsType != "bool") || (lhsType != "bool" && rhsType == "bool"))
                    {
                        ctx.LogRule("Rule_VariableInitializer", "as-err-implicit-conversion", sym);
                        ctx.Emit(sym, "as-err-implicit-conversion", rhsType);
                    }
                }
            }
        }
    }

    void ValidateVariable(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (Rule_VariableName(sym, ctx))
        {
            return;
        }

        const auto &sig = sym.GetVariable();
        if (sig.isLocal)
        {
            return;
        }

        if (sig.isVirtualProperty)
        {
            ValidateProperty(sym, ctx);
            return;
        }

        if (!sig.hasSemicolon)
        {
            ctx.LogRule("ValidateVariable", "as-syntax-error-missing", sym);
            ctx.Emit(sym, "as-syntax-error-missing", ";");
        }

        Rule_VariableModifiers(sym, ctx);
        Rule_VariableTypeResolution(sym, ctx);
        Rule_VariableInitializer(sym, ctx);
    }
}
