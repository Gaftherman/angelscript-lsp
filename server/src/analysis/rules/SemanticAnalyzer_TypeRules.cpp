#include "analysis/rules/TypeRules.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/SemanticHelpers.h"
#include <ankerl/unordered_dense.h>

namespace angel_lsp::analysis::rules
{
    static bool Rule_DuplicateTypeConflict(const std::vector<Symbol> &symbols, const DiagnosticContext &ctx)
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
                if (sym.fileUri != ctx.request.fileUri)
                    continue;
                if ((sym.type == SymbolType::Function || sym.type == SymbolType::Variable) &&
                    &sym != typeDefiningSymbol)
                {
                    const std::string typeName = SymbolTypeToString(typeDefiningSymbol->type);
                    ctx.LogRule("Rule_DuplicateTypeConflict", "as-err-name-conflict", sym);
                    ctx.Emit(sym, "as-err-name-conflict", sym.name, typeName);
                    hasConflict = true;
                }
            }
            if (hasConflict)
                return true;
        }

        return false;
    }

    static bool Rule_DuplicateVarCallableCollision(const std::vector<Symbol> &symbols, const DiagnosticContext &ctx)
    {
        bool hasFunction = false;
        bool hasEnum = false;
        bool hasVariable = false;
        const Symbol *varSym = nullptr;

        for (const auto &s : symbols)
        {
            if (s.fileUri != ctx.request.fileUri) continue;
            if (s.type == SymbolType::Function) hasFunction = true;
            if (s.type == SymbolType::Enum) hasEnum = true;
            if (s.type == SymbolType::Variable) { hasVariable = true; varSym = &s; }
        }

        if (hasFunction && hasVariable && varSym)
        {
            ctx.LogRule("Rule_DuplicateVarCallableCollision", "as-err-name-conflict", *varSym);
            ctx.Emit(*varSym, "as-err-name-conflict", varSym->name, "function");
            return true;
        }
        if (hasEnum && hasVariable && varSym)
        {
            ctx.LogRule("Rule_DuplicateVarCallableCollision", "as-err-name-conflict", *varSym);
            ctx.Emit(*varSym, "as-err-name-conflict", varSym->name, "named type");
            return true;
        }

        return false;
    }

    static void Rule_DuplicateSignature(const std::vector<Symbol> &symbols, const DiagnosticContext &ctx)
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
                if (sym.fileUri == ctx.request.fileUri)
                    currentFileSymbols.push_back(&sym);
            }

            for (size_t i = 1; i < currentFileSymbols.size(); ++i)
            {
                ctx.LogRule("Rule_DuplicateSignature", "as-err-duplicate-symbol", *currentFileSymbols[i]);
                ctx.Emit(*currentFileSymbols[i], "as-err-duplicate-symbol", currentFileSymbols[i]->name);
            }
        }

        if (allSameType && (firstType == SymbolType::Function || firstType == SymbolType::Funcdef))
        {
            if (firstType == SymbolType::Funcdef)
            {
                std::vector<const Symbol *> currentFileSymbols;
                for (const auto &sym : symbols)
                {
                    if (sym.fileUri == ctx.request.fileUri)
                        currentFileSymbols.push_back(&sym);
                }

                for (size_t i = 1; i < currentFileSymbols.size(); ++i)
                {
                    ctx.LogRule("Rule_DuplicateSignature", "as-err-duplicate-symbol", *currentFileSymbols[i]);
                    ctx.Emit(*currentFileSymbols[i], "as-err-duplicate-symbol", currentFileSymbols[i]->name);
                }
                return;
            }

            for (size_t i = 0; i < symbols.size(); ++i)
            {
                if (symbols[i].fileUri != ctx.request.fileUri)
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
                        ctx.LogRule("Rule_DuplicateSignature", "as-err-duplicate-symbol", symbols[i]);
                        ctx.Emit(symbols[i], "as-err-duplicate-symbol", symbols[i].name);
                        break;
                    }
                }
            }
        }
    }

    void ValidateDuplicates(const std::string &qualifiedName, const std::vector<Symbol> &symbols, const DiagnosticContext &ctx)
    {
        if (symbols.size() <= 1)
            return;

        if (symbols[0].type == SymbolType::Namespace)
            return;

        if (Rule_DuplicateTypeConflict(symbols, ctx))
            return;

        if (Rule_DuplicateVarCallableCollision(symbols, ctx))
            return;

        Rule_DuplicateSignature(symbols, ctx);
    }

    static bool Rule_InterfaceName(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            ctx.LogRule("Rule_InterfaceName", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return true;
        }

        return false;
    }

    static void Rule_InterfaceInheritance(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetInterface();

        if (sig.modifiers.isExternal)
        {
            ctx.LogRule("Rule_InterfaceInheritance", "as-err-external-not-found", sym);
            ctx.Emit(sym, "as-err-external-not-found", sym.name);
        }

        for (const auto &ifaceName : sig.inheritedInterfaces)
        {
            if (ifaceName == sym.name || !ctx.request.symbolTable.HasSymbol(ifaceName))
            {
                ctx.LogRule("Rule_InterfaceInheritance", "as-err-base-not-found", sym);
                ctx.Emit(sym, "as-err-base-not-found", ifaceName);
            }
        }
    }

    static void Rule_InterfaceMethods(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const std::string ifaceContainer = sym.qualifiedName;
        ctx.request.symbolTable.ForEachSymbol(
            [&](const std::string & /*qName*/, const std::vector<Symbol> &symsInTable)
            {
                for (const auto &ms : symsInTable)
                {
                    if (ms.containerName == ifaceContainer && ms.fileUri == ctx.request.fileUri)
                    {
                        if (ms.type == SymbolType::Function)
                        {
                            const auto &fnSig = ms.GetFunction();
                            if (fnSig.modifiers.access == AccessModifier::Private || fnSig.modifiers.access == AccessModifier::Protected)
                            {
                                ctx.LogRule("Rule_InterfaceMethods", "as-err-interface-private-method", ms);
                                ctx.Emit(ms, "as-err-interface-private-method", ms.name);
                            }
                            if (ms.name == sym.name || (!ms.name.empty() && ms.name[0] == '~'))
                            {
                                ctx.LogRule("Rule_InterfaceMethods", "as-err-interface-constructor", ms);
                                ctx.Emit(ms, "as-err-interface-constructor", ms.name);
                            }
                        }
                    }
                }
            });
    }

    void ValidateInterface(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (Rule_InterfaceName(sym, ctx))
        {
            return;
        }

        Rule_InterfaceInheritance(sym, ctx);
        Rule_InterfaceMethods(sym, ctx);
    }

    static bool Rule_TypedefName(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            ctx.LogRule("Rule_TypedefName", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return true;
        }

        return false;
    }

    static void Rule_TypedefTypeResolution(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetTypedef();

        if (sig.typeKind == TypeKind::Void || sig.baseType == "void" || !IsPrimitiveTypeName(sig.baseType))
        {
            ctx.LogRule("Rule_TypedefTypeResolution", "as-err-typedef-non-primitive", sym);
            ctx.Emit(sym, "as-err-typedef-non-primitive", sig.baseType);
        }
        else if (sig.typeKind == TypeKind::Unknown && !sig.baseType.empty())
        {
            if (!ctx.request.symbolTable.HasSymbol(sig.baseType))
            {
                ctx.LogRule("Rule_TypedefTypeResolution", "as-err-typedef-unresolved", sym);
                ctx.Emit(sym, "as-err-typedef-unresolved", sig.baseType);
            }
        }
    }

    void ValidateTypedef(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (Rule_TypedefName(sym, ctx))
        {
            return;
        }

        Rule_TypedefTypeResolution(sym, ctx);
    }

    static bool Rule_FuncdefName(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        const auto &sig = sym.GetFunction();
        if (sig.modifiers.isExternal)
        {
            ctx.LogRule("Rule_FuncdefName", "as-err-external-not-found", sym);
            ctx.Emit(sym, "as-err-external-not-found", sym.name);
        }

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            ctx.LogRule("Rule_FuncdefName", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return true;
        }

        return false;
    }

    static void Rule_FuncdefReturn(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();
        const auto &sig = sym.GetFunction();

        if (sig.returnHasPrimitiveHandle)
        {
            ctx.LogRule("Rule_FuncdefReturn", "as-err-handle-on-primitive", sym);
            ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
        }

        if (!sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" && !IsPrimitiveTypeName(sig.returnBaseTypeName) && sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName)
        {
            if (!ctx.request.symbolTable.HasSymbolAnywhere(sig.returnBaseTypeName))
            {
                ctx.LogRule("Rule_FuncdefReturn", "as-err-unresolved-type", sym);
                ctx.Emit(sym, "as-err-unresolved-type", sig.returnBaseTypeName);
            }
        }
    }

    void ValidateFuncdef(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (Rule_FuncdefName(sym, ctx))
        {
            return;
        }

        const auto &sig = sym.GetFunction();
        Rule_FuncdefReturn(sym, ctx);
        ValidateFunctionParameters(sym, sig, ctx);
    }

    static bool Rule_EnumName(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == stringTypeName || sym.name == arrayTypeName)
        {
            ctx.LogRule("Rule_EnumName", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return true;
        }

        return false;
    }

    static void Rule_EnumMembers(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetEnum();

        if (!sig.hasBraces && sig.members.empty() && !sig.modifiers.isExternal)
        {
            ctx.LogRule("Rule_EnumMembers", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (sig.modifiers.isExternal)
        {
            ctx.LogRule("Rule_EnumMembers", "as-err-external-not-found", sym);
            ctx.Emit(sym, "as-err-external-not-found", sym.name);
        }

        ankerl::unordered_dense::set<std::string> seenEnumMembers;
        for (const auto &member : sig.members)
        {
            if (!seenEnumMembers.insert(member.name).second)
            {
                ctx.LogRule("Rule_EnumMembers", "as-err-name-conflict", sym);
                ctx.Emit(sym, "as-err-name-conflict", member.name, "enum member");
            }

            if (!member.value.empty())
            {
                std::string val = member.value;
                if (val == member.name)
                {
                    ctx.LogRule("Rule_EnumMembers", "as-err-unresolved-symbol", sym);
                    ctx.Emit(sym, "as-err-unresolved-symbol", member.name);
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
                        ctx.LogRule("Rule_EnumMembers", "as-err-enum-invalid-initializer", sym);
                        ctx.Emit(sym, "as-err-enum-invalid-initializer", member.name);
                    }
                }
            }
        }
    }

    void ValidateEnum(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (Rule_EnumName(sym, ctx))
        {
            return;
        }

        Rule_EnumMembers(sym, ctx);
    }

    static bool Rule_NamespaceName(const Symbol &sym, const DiagnosticContext &ctx)
    {
        std::string_view arrayTN = ctx.request.GetArrayTypeName();
        std::string_view stringTN = ctx.request.GetStringTypeName();
        if (IsReservedKeyword(sym.name) || sym.name == arrayTN || sym.name == stringTN || IsPrimitiveTypeName(sym.name))
        {
            ctx.LogRule("Rule_NamespaceName", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return true;
        }

        return false;
    }

    void ValidateNamespace(const Symbol &sym, const DiagnosticContext &ctx)
    {
        Rule_NamespaceName(sym, ctx);
    }
}
