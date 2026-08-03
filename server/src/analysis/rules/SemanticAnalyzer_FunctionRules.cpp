#include "analysis/rules/FunctionRules.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/SemanticHelpers.h"

namespace angel_lsp::analysis::rules
{
    FunctionContext BuildFunctionContext(const Symbol &sym, const SemanticAnalysisRequest &req)
    {
        FunctionContext ctx;
        std::string unqualifiedContainer = sym.containerName;
        size_t lastColons = unqualifiedContainer.rfind("::");
        if (lastColons != std::string::npos)
        {
            unqualifiedContainer = unqualifiedContainer.substr(lastColons + 2);
        }
        ctx.isCtor = (!sym.containerName.empty() && (sym.name == sym.containerName || sym.name == unqualifiedContainer));
        ctx.isDtor = (!sym.name.empty() && sym.name[0] == '~');

        if (!sym.containerName.empty())
        {
            auto parentOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt)
            {
                if (parentOpt->type == SymbolType::Class || parentOpt->type == SymbolType::Interface)
                {
                    ctx.isInsideClass = true;
                    if (parentOpt->type == SymbolType::Interface)
                    {
                        ctx.isInterface = true;
                    }
                    else if (parentOpt->type == SymbolType::Class && parentOpt->GetClass().modifiers.isMixin)
                    {
                        ctx.isInsideMixin = true;
                    }
                }
            }
        }
        return ctx;
    }

    /**
     * @brief Validates function name against reserved keywords and registered types.
     * @return true if an error is detected (stops further signature validation), false if OK.
     */
    static bool Rule_FunctionName(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        if (sym.containerName.empty() && (sym.name == arrayTypeName || sym.name == stringTypeName))
        {
            ctx.LogRule("Rule_FunctionName", "as-err-name-conflict", sym);
            ctx.Emit(sym, "as-err-name-conflict", sym.name, "registered object type");
            return true;
        }

        if (IsReservedKeyword(sym.name))
        {
            ctx.LogRule("Rule_FunctionName", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return true;
        }

        return false;
    }

    static void Rule_FunctionBody(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        if (!sig.hasBody && !sig.isInterfaceMethod && !sig.modifiers.isExternal && !sig.modifiers.isDelete)
        {
            ctx.LogRule("Rule_FunctionBody", "as-err-missing-body", sym);
            ctx.Emit(sym, "as-err-missing-body", sym.name);
        }

        if ((sig.modifiers.isDelete || sig.modifiers.isExternal) && sig.hasBody)
        {
            ctx.LogRule("Rule_FunctionBody", "as-err-delete-with-body", sym);
            ctx.Emit(sym, "as-err-delete-with-body", sym.name);
        }

        if (!sym.containerName.empty() && sig.returnType.empty() && !fctx.isCtor && !fctx.isDtor)
        {
            ctx.LogRule("Rule_FunctionBody", "as-err-missing-body", sym);
            ctx.Emit(sym, "as-err-missing-body", sym.name);
        }
    }

    static void Rule_FunctionReturnType(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();
        const auto &sig = sym.GetFunction();

        if (sig.modifiers.isReturnReference && !sig.returnIsConst && (IsPrimitiveTypeName(sig.returnBaseTypeName) || sig.returnBaseTypeName == stringTypeName))
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-invalid-reference-return", sym);
            ctx.Emit(sym, "as-err-invalid-reference-return", sig.returnBaseTypeName);
        }

        if ((sig.returnHasPrimitiveHandle && !sig.returnIsArray && sig.returnType.find("[]") == std::string::npos && sig.returnType.find("array<") == std::string::npos) || (sig.modifiers.isHandle && sig.returnBaseTypeName == stringTypeName && !sig.returnIsArray))
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-handle-on-primitive", sym);
            ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
        }

        if (sig.returnBaseTypeName == "auto")
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-unresolved-type", sym);
            ctx.Emit(sym, "as-err-unresolved-type", "auto");
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.returnIsConst)
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-const-void-return", sym);
            ctx.Emit(sym, "as-err-const-void-return");
        }

        if (!sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" && !IsPrimitiveTypeName(sig.returnBaseTypeName) && sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName && !ctx.request.symbolTable.HasSymbolAnywhere(sig.returnBaseTypeName))
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-unresolved-type", sym);
            ctx.Emit(sym, "as-err-unresolved-type", sig.returnBaseTypeName);
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.modifiers.isReturnReference)
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-void-reference", sym);
            ctx.Emit(sym, "as-err-void-reference");
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.hasValueReturn)
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (IsPrimitiveTypeName(sig.returnBaseTypeName) && !sig.returnIsArray && sig.returnTypeKind != TypeKind::Array && sig.returnType.find("[]") == std::string::npos && sig.returnType.find("array<") == std::string::npos && sig.returnExpression == "null")
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-handle-on-primitive", sym);
            ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
        }

        if (sig.returnBaseTypeName != "void" && sig.returnTypeKind != TypeKind::Void && !sig.returnBaseTypeName.empty() && !fctx.isCtor && (!sym.name.empty() && sym.name[0] != '~'))
        {
            if (sig.hasBody && !sig.hasValueReturn && !sig.hasEmptyReturn)
            {
                ctx.LogRule("Rule_FunctionReturnType", "as-err-not-all-paths-return", sym);
                ctx.Emit(sym, "as-err-not-all-paths-return", sym.name);
            }
        }
    }

    static void Rule_FunctionModifiers(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        if (sig.modifiers.isFinal && !fctx.isInsideClass)
        {
            ctx.LogRule("Rule_FunctionModifiers", "as-err-global-function-qualifiers", sym);
            ctx.Emit(sym, "as-err-global-function-qualifiers", sym.name);
        }
        if (sig.modifiers.isOverride && !fctx.isInsideClass)
        {
            ctx.LogRule("Rule_FunctionModifiers", "as-err-global-function-qualifiers", sym);
            ctx.Emit(sym, "as-err-global-function-qualifiers", sym.name);
        }
        if (sig.modifiers.isConst && !fctx.isInsideClass)
        {
            ctx.LogRule("Rule_FunctionModifiers", "as-err-global-function-qualifiers", sym);
            ctx.Emit(sym, "as-err-global-function-qualifiers", sym.name);
        }

        if (sig.modifiers.isExternal && sig.modifiers.isFinal)
        {
            ctx.LogRule("Rule_FunctionModifiers", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (sig.modifiers.isExternal && sig.modifiers.isOverride)
        {
            ctx.LogRule("Rule_FunctionModifiers", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }
    }

    static void Rule_CtorDtor(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        if (fctx.isCtor || fctx.isDtor)
        {
            if (sig.modifiers.isConst)
            {
                ctx.LogRule("Rule_CtorDtor", "as-syntax-error", sym);
                ctx.Emit(sym, "as-syntax-error");
            }
            if (sig.modifiers.isFinal || sig.modifiers.isAbstract)
            {
                ctx.LogRule("Rule_CtorDtor", "as-syntax-error", sym);
                ctx.Emit(sym, "as-syntax-error");
            }

            if (fctx.isDtor)
            {
                if (!sig.parameters.empty())
                {
                    ctx.LogRule("Rule_CtorDtor", "as-err-destructor-param", sym);
                    ctx.Emit(sym, "as-err-destructor-param", sym.name);
                }
                if (!sig.returnType.empty() && sig.returnType != "void")
                {
                    ctx.LogRule("Rule_CtorDtor", "as-err-destructor-return-type", sym);
                    ctx.Emit(sym, "as-err-destructor-return-type", sym.name);
                }
                if (sig.modifiers.isDelete)
                {
                    ctx.LogRule("Rule_CtorDtor", "as-err-destructor-delete", sym);
                    ctx.Emit(sym, "as-err-destructor-delete", sym.name);
                }
            }
            else if (fctx.isCtor)
            {
                if (!sig.returnType.empty() && sig.returnType != "void")
                {
                    ctx.LogRule("Rule_CtorDtor", "as-syntax-error", sym);
                    ctx.Emit(sym, "as-syntax-error");
                }
            }
        }

        if (sig.modifiers.isDelete && (sig.modifiers.isConst || sig.modifiers.isFinal || sig.modifiers.isOverride || sig.modifiers.isAbstract))
        {
            ctx.LogRule("Rule_FunctionModifiers", "as-err-delete-with-other-qualifier", sym);
            ctx.Emit(sym, "as-err-delete-with-other-qualifier", sym.name);
        }
    }


    static void Rule_FunctionBodyFlow(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        for (const auto &labelName : sig.gotoTargetLabels)
        {
            std::string targetLabel = labelName + ":";
            if (sig.defaultValue.find(targetLabel) == std::string::npos)
            {
                ctx.LogRule("Rule_FunctionBodyFlow", "as-err-unresolved-symbol", sym);
                ctx.Emit(sym, "as-err-unresolved-symbol", labelName);
            }
        }
    }

    static void Rule_FunctionBodyCast(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        for (const auto &castType : sig.bodyCastTypes)
        {
            if (castType == "void")
            {
                ctx.LogRule("Rule_FunctionBodyCast", "as-err-unresolved-type", sym);
                ctx.Emit(sym, "as-err-unresolved-type", "cast");
            }
        }
    }

    static void Rule_FunctionBodyScope(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        for (const auto &qualifiedName : sig.bodyQualifiedNames)
        {
            size_t scopePos = qualifiedName.find("::");
            if (scopePos != std::string::npos)
            {
                std::string scopePrefix = qualifiedName.substr(0, scopePos);
                if (scopePrefix != "global" && qualifiedName.size() > scopePos + 2)
                {
                    if (!ctx.request.symbolTable.HasSymbolAnywhere(qualifiedName))
                    {
                        ctx.LogRule("Rule_FunctionBodyScope", "as-syntax-error", sym);
                        ctx.Emit(sym, "as-syntax-error");
                        break;
                    }
                }
            }
        }
    }

    static void Rule_FunctionReturnExpr(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();
        const auto &sig = sym.GetFunction();

        if (sig.hasValueReturn && IsReservedKeyword(sig.returnExpression) && sig.returnExpression != "null" && sig.returnExpression != "true" && sig.returnExpression != "false")
        {
            ctx.LogRule("Rule_FunctionReturnExpr", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (sig.hasValueReturn && !sig.returnCallTargetName.empty())
        {
            const std::string &target = sig.returnCallTargetName;
            if (target.find("::") == std::string::npos &&
                !IsPrimitiveTypeName(target) && target != stringTypeName && target != arrayTypeName &&
                !target.starts_with("array<") && !target.starts_with(std::string(arrayTypeName) + "<") &&
                target != "null" && target != "true" && target != "false")
            {
                std::string expectedQN = sym.containerName.empty() ? target : sym.containerName + "::" + target;
                if (!ctx.request.symbolTable.HasSymbol(expectedQN) && !ctx.request.symbolTable.HasSymbol(target))
                {
                    ctx.LogRule("Rule_FunctionReturnExpr", "as-err-unresolved-type", sym);
                    ctx.Emit(sym, "as-err-unresolved-type", target);
                }
            }
        }

        if (sig.hasSuperCall)
        {
            if (!sym.containerName.empty())
            {
                const auto *classSyms = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
                if (classSyms)
                {
                    for (const auto &cSym : *classSyms)
                    {
                        if (cSym.type == SymbolType::Class && cSym.GetClass().bases.empty())
                        {
                            ctx.LogRule("Rule_FunctionReturnExpr", "as-syntax-error", sym);
                            ctx.Emit(sym, "as-syntax-error");
                        }
                    }
                }
            }
        }
    }

    static void Rule_FunctionOverride(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        if (sig.modifiers.isOverride && fctx.isInsideClass)
        {
            bool hasBaseMethod = false;
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
                            std::string methodQN = bSym.qualifiedName.empty() ? sym.name : bSym.qualifiedName + "::" + sym.name;
                            if (ctx.request.symbolTable.HasSymbol(methodQN))
                            {
                                hasBaseMethod = true;
                                break;
                            }
                        }
                    }
                    if (hasBaseMethod) break;
                }
            }
            if (!hasBaseMethod)
            {
                ctx.LogRule("Rule_FunctionOverride", "as-err-override-no-base", sym);
                ctx.Emit(sym, "as-err-override-no-base", sym.name, sym.containerName);
            }
        }
    }

    static void Rule_OperatorOverload(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        if (sym.name.rfind("op", 0) == 0)
        {
            if (sym.name == "opIndex" || sym.name == "opCall" || sym.name == "opCast")
            {
                if (!fctx.isInsideClass)
                {
                    ctx.LogRule("Rule_OperatorOverload", "as-err-global-operator-overload", sym);
                    ctx.Emit(sym, "as-err-global-operator-overload", sym.name);
                }
            }
        }
    }

    static void Rule_MixinConstraints(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        if (fctx.isInsideMixin)
        {
            if (fctx.isCtor)
            {
                ctx.LogRule("Rule_MixinConstraints", "as-err-mixin-constructor", sym);
                ctx.Emit(sym, "as-err-mixin-constructor", sym.name);
            }
            if (fctx.isDtor)
            {
                ctx.LogRule("Rule_MixinConstraints", "as-err-mixin-destructor", sym);
                ctx.Emit(sym, "as-err-mixin-destructor", sym.name);
            }

            bool isMixinMember = false;
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
                            if (bSym.type == SymbolType::Class && bSym.GetClass().modifiers.isMixin)
                            {
                                isMixinMember = true;
                                break;
                            }
                        }
                    }
                    if (isMixinMember) break;
                }
            }
            if (!isMixinMember)
            {
                auto symsInContainer = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
                if (symsInContainer)
                {
                    isMixinMember = true;
                }
            }
            bool isAutoGeneratable = isMixinMember || (sym.name == "opAssign" || sym.name == "opEquals" || sym.name == "opCmp" || (!sym.containerName.empty() && (sym.name == sym.containerName || sym.name == "~" + sym.containerName)));
            if (!isAutoGeneratable)
            {
                ctx.LogRule("Rule_MixinConstraints", "as-syntax-error", sym);
                ctx.Emit(sym, "as-syntax-error");
            }
        }

        if (!sym.containerName.empty())
        {
            auto parentOpt = ctx.request.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && parentOpt->type == SymbolType::Class && parentOpt->GetClass().modifiers.isMixin)
            {
                if (fctx.isCtor)
                {
                    ctx.LogRule("Rule_MixinConstraints", "as-err-mixin-constructor", sym);
                    ctx.Emit(sym, "as-err-mixin-constructor", sym.name);
                }
                else if (fctx.isDtor)
                {
                    ctx.LogRule("Rule_MixinConstraints", "as-err-mixin-destructor", sym);
                    ctx.Emit(sym, "as-err-mixin-destructor", sym.name);
                }
            }
        }
    }

    bool ValidateFunctionSignature(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        if (Rule_FunctionName(sym, fctx, ctx))
        {
            return false;
        }

        Rule_FunctionReturnType(sym, fctx, ctx);
        Rule_FunctionModifiers(sym, fctx, ctx);
        Rule_CtorDtor(sym, fctx, ctx);
        Rule_FunctionOverride(sym, fctx, ctx);
        Rule_OperatorOverload(sym, fctx, ctx);
        Rule_MixinConstraints(sym, fctx, ctx);

        ValidateFunctionParameters(sym, sym.GetFunction(), ctx);
        return true;
    }

    void ValidateFunctionBody(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        Rule_FunctionBody(sym, fctx, ctx);
        Rule_FunctionBodyFlow(sym, fctx, ctx);
        Rule_FunctionBodyCast(sym, fctx, ctx);
        Rule_FunctionBodyScope(sym, fctx, ctx);
        Rule_FunctionReturnExpr(sym, fctx, ctx);
    }

    void ValidateFunction(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const FunctionContext fctx = BuildFunctionContext(sym, ctx.request);

        if (!ValidateFunctionSignature(sym, fctx, ctx))
        {
            return;
        }

        ValidateFunctionBody(sym, fctx, ctx);
    }

    void ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        ankerl::unordered_dense::set<std::string> seenParamNames;
        bool seenDefault = false;
        bool isExternalFunc = sig.modifiers.isExternal;

        for (const auto &param : sig.parameters)
        {
            if (isExternalFunc && param.modifier == ParameterModifier::InOut)
            {
                ctx.LogParam("ValidateFunctionParameters", "as-syntax-error", param, sym);
                ctx.Emit(param, sym, "as-syntax-error");
            }

            if (isExternalFunc && !param.baseTypeName.empty() && !IsPrimitiveTypeName(param.baseTypeName) && param.baseTypeName != stringTypeName && param.baseTypeName != arrayTypeName && !param.baseTypeName.starts_with("array<") && !ctx.request.symbolTable.HasSymbolAnywhere(param.baseTypeName))
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-unresolved-type", param, sym);
                ctx.Emit(param, sym, "as-err-unresolved-type", param.baseTypeName);
            }

            if (!param.defaultValue.empty())
            {
                seenDefault = true;
            }
            else if (seenDefault && sym.type != SymbolType::Funcdef)
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-default-param-order", param, sym);
                ctx.Emit(param, sym, "as-err-default-param-order", param.name, sym.name);
            }

            if (param.typeKind == TypeKind::Void || param.baseTypeName == "void")
            {
                bool isUnnamedVoid = (sig.parameters.size() == 1 && param.name.empty());
                if (!isUnnamedVoid && sym.type != SymbolType::Funcdef)
                {
                    ctx.LogParam("ValidateFunctionParameters", "as-err-void-parameter", param, sym);
                    ctx.Emit(param, sym, "as-err-void-parameter", param.name, sym.name);
                }
            }

            if (param.baseTypeName == "auto")
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-unresolved-type", param, sym);
                ctx.Emit(param, sym, "as-err-unresolved-type", "auto");
            }

            if (param.modifier == ParameterModifier::InOut &&
                (param.isHandle || IsPrimitiveTypeName(param.baseTypeName) || param.baseTypeName == stringTypeName || param.typeKind == TypeKind::String || param.typeKind == TypeKind::Int32 || param.typeKind == TypeKind::Float || param.typeKind == TypeKind::Bool))
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-inout-on-primitive", param, sym);
                ctx.Emit(param, sym, "as-err-inout-on-primitive", param.baseTypeName);
            }

            if (param.hasDoubleReference)
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-double-reference", param, sym);
                ctx.Emit(param, sym, "as-err-double-reference", param.baseTypeName);
            }

            bool isStandaloneRef = param.isStandaloneRef;
            if (isStandaloneRef && (IsPrimitiveTypeName(param.baseTypeName) || param.typeKind == TypeKind::Int32 || param.typeKind == TypeKind::Float || param.typeKind == TypeKind::Bool || param.typeKind == TypeKind::Double || param.typeKind == TypeKind::UInt32 || param.baseTypeName == stringTypeName))
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-standalone-reference", param, sym);
                ctx.Emit(param, sym, "as-err-standalone-reference", param.name);
            }
            else if (isStandaloneRef && !param.defaultValue.empty())
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-invalid-reference-return", param, sym);
                ctx.Emit(param, sym, "as-err-invalid-reference-return", param.baseTypeName);
            }

            if (!param.name.empty())
            {
                if (IsReservedKeyword(param.name))
                {
                    ctx.LogParam("ValidateFunctionParameters", "as-err-reserved-keyword-name", param, sym);
                    ctx.Emit(param, sym, "as-err-reserved-keyword-name", param.name);
                }
                if (!seenParamNames.insert(param.name).second)
                {
                    ctx.LogParam("ValidateFunctionParameters", "as-err-duplicate-param", param, sym);
                    ctx.Emit(param, sym, "as-err-duplicate-param", param.name, sym.name);
                }
                else
                {
                    const auto *globalSyms = ctx.request.symbolTable.FindSymbolsPtr(param.name);
                    if (globalSyms)
                    {
                        for (const auto &gSym : *globalSyms)
                        {
                            if (gSym.containerName.empty() && gSym.type == SymbolType::Variable)
                            {
                                ctx.LogParam("ValidateFunctionParameters", "as-warn-shadow-global", param, sym);
                                ctx.Emit(param, sym, "as-warn-shadow-global", param.name, DiagnosticSeverity::Warning);
                                break;
                            }
                        }
                    }
                }
            }

            if (param.hasPrimitiveHandle || (param.baseTypeName == stringTypeName && param.isHandle))
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-handle-on-primitive", param, sym);
                ctx.Emit(param, sym, "as-err-handle-on-primitive", param.baseTypeName);
            }

            if (!param.baseTypeName.empty() && !IsPrimitiveTypeName(param.baseTypeName) && param.baseTypeName != stringTypeName && param.baseTypeName != arrayTypeName && !ctx.request.symbolTable.HasSymbol(param.baseTypeName) && !ctx.request.symbolTable.HasSymbolAnywhere(param.baseTypeName))
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-unresolved-type", param, sym);
                ctx.Emit(param, sym, "as-err-unresolved-type", param.baseTypeName);
            }

            if (!param.isHandle)
            {
                const auto *typeSyms = ctx.request.symbolTable.FindSymbolsPtr(param.baseTypeName);
                if (typeSyms)
                {
                    for (const auto &tSym : *typeSyms)
                    {
                        if (tSym.type == SymbolType::Funcdef)
                        {
                            ctx.LogParam("ValidateFunctionParameters", "as-err-funcdef-not-handle", param, sym);
                            ctx.Emit(param, sym, "as-err-funcdef-not-handle", param.baseTypeName, param.baseTypeName);
                            break;
                        }
                    }
                }
            }
        }
    }
}
