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

        if (IsMixinClass(sig.returnBaseTypeName, ctx.request.symbolTable))
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-mixin-not-a-type", sym);
            ctx.Emit(sym, "as-err-mixin-not-a-type", sig.returnBaseTypeName);
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

        if (!sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" && !IsKnownType(sig.returnBaseTypeName, ctx))
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-unresolved-type", sym);
            ctx.Emit(sym, "as-err-unresolved-type", sig.returnBaseTypeName);
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.modifiers.isReturnReference)
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-void-reference", sym);
            ctx.Emit(sym, "as-err-void-reference");
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.bodyAnalysis && sig.bodyAnalysis->hasValueReturn)
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (IsPrimitiveTypeName(sig.returnBaseTypeName) && !sig.returnIsArray && sig.returnTypeKind != TypeKind::Array && sig.returnType.find("[]") == std::string::npos && sig.returnType.find("array<") == std::string::npos && sig.bodyAnalysis && sig.bodyAnalysis->returnExpression == "null")
        {
            ctx.LogRule("Rule_FunctionReturnType", "as-err-handle-on-primitive", sym);
            ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
        }

        if (sig.returnBaseTypeName != "void" && sig.returnTypeKind != TypeKind::Void && !sig.returnBaseTypeName.empty() && !fctx.isCtor && (!sym.name.empty() && sym.name[0] != '~'))
        {
            if (sig.bodyAnalysis && !sig.bodyAnalysis->hasValueReturn && !sig.bodyAnalysis->hasEmptyReturn)
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

        if (sig.modifiers.isDelete)
        {
            if (!fctx.isInsideClass || sig.hasBody || (sig.defaultValue.find("= delete") == std::string::npos && sig.defaultValue.find("=delete") == std::string::npos))
            {
                ctx.LogRule("Rule_FunctionModifiers", "as-syntax-error", sym);
                ctx.Emit(sym, "as-syntax-error");
            }
        }

        if (sig.modifiers.isProperty && sig.hasBody)
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
            if (fctx.isCtor)
            {
                if (sig.modifiers.isConst || sig.modifiers.isFinal || sig.modifiers.isAbstract)
                {
                    ctx.LogRule("Rule_CtorDtor", "as-syntax-error", sym);
                    ctx.Emit(sym, "as-syntax-error");
                }
                if (!sig.returnType.empty() && sig.returnType != "void")
                {
                    ctx.LogRule("Rule_CtorDtor", "as-syntax-error", sym);
                    ctx.Emit(sym, "as-syntax-error");
                }
            }
            else if (fctx.isDtor)
            {
                if (!sig.parameters.empty())
                {
                    ctx.LogRule("Rule_CtorDtor", "as-err-destructor-param", sym);
                    ctx.Emit(sym, "as-err-destructor-param", sym.name);
                }
                if (!sig.returnType.empty() || (sig.bodyAnalysis && sig.bodyAnalysis->hasValueReturn))
                {
                    ctx.LogRule("Rule_CtorDtor", "as-err-destructor-return-type", sym);
                    ctx.Emit(sym, "as-err-destructor-return-type", sym.name);
                }
                if (sig.modifiers.isDelete)
                {
                    ctx.LogRule("Rule_CtorDtor", "as-err-destructor-delete", sym);
                    ctx.Emit(sym, "as-err-destructor-delete", sym.name);
                }
                if (sig.modifiers.isShared || sig.modifiers.isExternal)
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
        if (!sig.bodyAnalysis)
            return;
        const auto &body = *sig.bodyAnalysis;

        for (const auto &labelName : body.gotoTargetLabels)
        {
            std::string targetLabel = labelName + ":";
            if (sig.defaultValue.find(targetLabel) == std::string::npos)
            {
                ctx.LogRule("Rule_FunctionBodyFlow", "as-err-unresolved-symbol", sym);
                ctx.Emit(sym, "as-err-unresolved-symbol", labelName);
            }
        }

        for (const auto &range : body.invalidBreakStatements)
        {
            ctx.LogRule("Rule_FunctionBodyFlow", "as-err-break-outside-loop", sym);
            ctx.EmitAtRange(sym, range, "as-err-break-outside-loop");
        }

        for (const auto &range : body.invalidContinueStatements)
        {
            ctx.LogRule("Rule_FunctionBodyFlow", "as-err-continue-outside-loop", sym);
            ctx.EmitAtRange(sym, range, "as-err-continue-outside-loop");
        }
    }

    static void Rule_FunctionBodySwitch(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();
        if (!sig.bodyAnalysis)
            return;
        const auto &body = *sig.bodyAnalysis;

        for (const auto &range : body.invalidDefaultStatements)
        {
            ctx.LogRule("Rule_FunctionBodySwitch", "as-err-default-must-be-last", sym);
            ctx.EmitAtRange(sym, range, "as-err-default-must-be-last");
        }

        for (const auto &item : body.invalidCaseStatements)
        {
            if (item.reason == "invalid_type")
            {
                ctx.LogRule("Rule_FunctionBodySwitch", "as-err-invalid-case-type", sym);
                ctx.EmitAtRange(sym, item.range, "as-err-invalid-case-type");
            }
            else if (item.reason == "duplicate_value")
            {
                ctx.LogRule("Rule_FunctionBodySwitch", "as-err-duplicate-case-value", sym);
                ctx.EmitAtRange(sym, item.range, "as-err-duplicate-case-value", item.valueText);
            }
        }
    }

    static void ValidateTypeInfoAndTemplateArgs(const TypeExtractionResult &info, const Symbol &sym, const DiagnosticContext &ctx, bool isTopLevelCastTarget)
    {
        std::string effectiveName = info.templateName.empty() ? info.baseTypeName : info.templateName;
        if (effectiveName.empty()) return;

        if (IsMixinClass(effectiveName, ctx.request.symbolTable))
        {
            ctx.LogRule("ValidateTypeAndTemplateArgs", "as-err-mixin-not-a-type", sym);
            ctx.Emit(sym, "as-err-mixin-not-a-type", effectiveName);
        }
        else if (effectiveName == "void" || effectiveName == "auto" || effectiveName == "null")
        {
            ctx.LogRule("ValidateTypeAndTemplateArgs", "as-err-unresolved-type", sym);
            ctx.Emit(sym, "as-err-unresolved-type", effectiveName);
        }
        else if (isTopLevelCastTarget && (IsPrimitiveTypeName(effectiveName) || effectiveName == ctx.request.GetStringTypeName()))
        {
            ctx.LogRule("ValidateTypeAndTemplateArgs", "as-err-unresolved-type", sym);
            ctx.Emit(sym, "as-err-unresolved-type", effectiveName);
        }
        else if (!IsKnownType(effectiveName, ctx))
        {
            bool isTemplatePlaceholder = false;
            if (!sym.containerName.empty())
            {
                auto cSyms = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
                if (cSyms)
                {
                    for (const auto &cS : *cSyms)
                    {
                        if (cS.type == SymbolType::Class && cS.GetClass().isTemplate)
                        {
                            isTemplatePlaceholder = true;
                            break;
                        }
                    }
                }
            }
            if (!isTemplatePlaceholder)
            {
                ctx.LogRule("ValidateTypeAndTemplateArgs", "as-err-unresolved-type", sym);
                ctx.Emit(sym, "as-err-unresolved-type", effectiveName);
            }
        }

        for (const auto &arg : info.templateArguments)
        {
            ValidateTypeInfoAndTemplateArgs(arg, sym, ctx, false);
        }
    }

    static std::string ExtractBaseTemplateName(const std::string &tName)
    {
        std::string base = tName;
        while (!base.empty() && (base.back() == '@' || base.back() == '&' || isspace(static_cast<unsigned char>(base.back()))))
        {
            base.pop_back();
        }
        if (base.rfind("const ", 0) == 0) base = base.substr(6);
        size_t anglePos = base.find('<');
        if (anglePos != std::string::npos) base = base.substr(0, anglePos);
        return base;
    }

    static std::string ExtractTemplateArgsString(const std::string &tName)
    {
        size_t anglePos = tName.find('<');
        if (anglePos == std::string::npos) return "";
        size_t lastAngle = tName.rfind('>');
        if (lastAngle != std::string::npos && lastAngle > anglePos)
        {
            return tName.substr(anglePos + 1, lastAngle - anglePos - 1);
        }
        return "";
    }

    static std::string NormalizeTypeArg(std::string arg)
    {
        while (!arg.empty() && (arg.back() == '@' || arg.back() == '&' || isspace(static_cast<unsigned char>(arg.back()))))
        {
            arg.pop_back();
        }
        while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t'))
        {
            arg = arg.substr(1);
        }
        if (arg.rfind("const ", 0) == 0) arg = arg.substr(6);
        return arg;
    }

    static bool AreTemplateArgsCompatible(const std::string &targetArgsStr, const std::string &operandArgsStr)
    {
        std::string normTarget = NormalizeTypeArg(targetArgsStr);
        std::string normOperand = NormalizeTypeArg(operandArgsStr);
        return normTarget == normOperand;
    }

    static void Rule_FunctionBodyCast(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();

        if (sig.bodyAnalysis)
        {
            const auto &body = *sig.bodyAnalysis;
            for (const auto &castExpr : body.bodyCastExpressions)
            {
                ValidateTypeInfoAndTemplateArgs(castExpr.targetTypeInfo, sym, ctx, true);
            }

            for (const auto &castExpr : body.bodyCastExpressions)
            {
                if (!castExpr.operandText.empty())
                {
                    std::string operandType;
                    for (const auto &vInfo : body.bodyVariableTypes)
                    {
                        if (!vInfo.varName.empty() && vInfo.varName == castExpr.operandText)
                        {
                            operandType = vInfo.fullType;
                            break;
                        }
                    }
                    if (operandType.empty())
                    {
                        for (const auto &param : sig.parameters)
                        {
                            if (param.name == castExpr.operandText)
                            {
                                operandType = param.typeName;
                                break;
                            }
                        }
                    }

                    if (!operandType.empty())
                    {
                        std::string targetBase = ExtractBaseTemplateName(castExpr.targetType);
                        std::string operandBase = ExtractBaseTemplateName(operandType);

                        if (!targetBase.empty() && targetBase == operandBase)
                        {
                            std::string targetArgs = ExtractTemplateArgsString(castExpr.targetType);
                            std::string operandArgs = ExtractTemplateArgsString(operandType);

                            if (!targetArgs.empty() && !operandArgs.empty())
                            {
                                if (!AreTemplateArgsCompatible(targetArgs, operandArgs))
                                {
                                    ctx.LogRule("Rule_FunctionBodyCast", "as-err-unresolved-type", sym);
                                    ctx.Emit(sym, "as-err-unresolved-type", castExpr.targetType);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (sig.defaultValue.find("cast<") != std::string::npos)
        {
            size_t cPos = sig.defaultValue.find("cast<");
            while (cPos != std::string::npos)
            {
                size_t rParen = sig.defaultValue.find(')', cPos);
                if (rParen != std::string::npos)
                {
                    std::string castExpr = sig.defaultValue.substr(cPos, rParen - cPos + 1);
                    size_t commaOrParen = castExpr.find('>');
                    if (commaOrParen != std::string::npos && commaOrParen + 1 < castExpr.size())
                    {
                        std::string argStr = castExpr.substr(commaOrParen + 1);
                        if (argStr.size() >= 2 && argStr.front() == '(') argStr = argStr.substr(1);
                        size_t endDigit = argStr.find_first_of("),;\t\r\n ");
                        if (endDigit != std::string::npos) argStr = argStr.substr(0, endDigit);
                        if (!argStr.empty() && isdigit(static_cast<unsigned char>(argStr.front())))
                        {
                            ctx.LogRule("Rule_FunctionBodyCast", "as-err-unresolved-type", sym);
                            ctx.Emit(sym, "as-err-unresolved-type", "cast");
                            break;
                        }
                    }
                }
                cPos = sig.defaultValue.find("cast<", cPos + 5);
            }
        }
    }


    static void Rule_FunctionBodyScope(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();
        if (!sig.bodyAnalysis)
            return;

        for (const auto &qualifiedName : sig.bodyAnalysis->bodyQualifiedNames)
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
        if (!sig.bodyAnalysis)
            return;

        if (sig.returnType != "void" && !sig.returnType.empty() && (!fctx.isCtor && !fctx.isDtor) && sig.bodyAnalysis->hasEmptyReturn)
        {
            ctx.LogRule("Rule_FunctionReturnExpr", "as-err-not-all-paths-return", sym);
            ctx.Emit(sym, "as-err-not-all-paths-return", sym.name);
        }

        if (sig.bodyAnalysis->hasValueReturn && IsReservedKeyword(sig.bodyAnalysis->returnExpression) && sig.bodyAnalysis->returnExpression != "null" && sig.bodyAnalysis->returnExpression != "true" && sig.bodyAnalysis->returnExpression != "false")
        {
            ctx.LogRule("Rule_FunctionReturnExpr", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error");
        }

        if (sig.bodyAnalysis->hasValueReturn && !sig.bodyAnalysis->returnCallTargetName.empty())
        {
            const std::string &target = sig.bodyAnalysis->returnCallTargetName;
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

        if (sig.bodyAnalysis->hasSuperCall)
        {
            if (!sym.containerName.empty())
            {
                auto classSyms = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
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

        if (sig.modifiers.isOverride && fctx.isInsideClass && !fctx.isDtor)
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

    static void Rule_FunctionBodyVariables(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();
        if (!sig.bodyAnalysis)
            return;

        for (const auto &varInfo : sig.bodyAnalysis->bodyVariableTypes)
        {
            if (IsMixinClass(varInfo.typeName, ctx.request.symbolTable))
            {
                ctx.LogRule("Rule_FunctionBodyVariables", "as-err-mixin-not-a-type", sym);
                ctx.EmitAtRange(sym, varInfo.range, "as-err-mixin-not-a-type", varInfo.typeName);
            }
        }
    }

    static void Rule_FunctionBodyUnresolved(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();
        if (!sig.bodyAnalysis)
        {
            return;
        }
        const auto &body = *sig.bodyAnalysis;

        std::unordered_set<std::string> localScope;
        for (const auto &param : sig.parameters)
        {
            if (!param.name.empty())
            {
                localScope.insert(param.name);
            }
        }
        for (const auto &vInfo : body.bodyVariableTypes)
        {
            if (!vInfo.varName.empty())
            {
                localScope.insert(vInfo.varName);
            }
        }

        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        for (const auto &ref : body.bodyIdentifierRefs)
        {
            if (ref.isMemberAccess)
            {
                continue;
            }

            const std::string &name = ref.name;

            if (name == "null" || name == "true" || name == "false" || name == "this" ||
                name == "super" || name == "cast" || name == "void" || name == "auto" ||
                name == "int" || name == "int8" || name == "int16" || name == "int32" || name == "int64" ||
                name == "uint" || name == "uint8" || name == "uint16" || name == "uint32" || name == "uint64" ||
                name == "float" || name == "double" || name == "bool" ||
                name == stringTypeName || name == arrayTypeName ||
                IsReservedKeyword(name))
            {
                continue;
            }

            if (localScope.contains(name))
            {
                continue;
            }

            if (ctx.request.IsRegisteredSymbol(name))
            {
                continue;
            }

            if (!sym.containerName.empty())
            {
                std::string memberQN = sym.containerName + "::" + name;
                if (ctx.request.symbolTable.HasSymbol(memberQN))
                {
                    continue;
                }
            }

            if (ctx.request.symbolTable.HasSymbolAnywhere(name) || ctx.request.symbolTable.HasSymbol(name))
            {
                continue;
            }

            ctx.LogRule("Rule_FunctionBodyUnresolved", "as-err-undeclared-identifier", sym);
            ctx.EmitAtRange(sym, ref.range, "as-err-undeclared-identifier", name);
        }
    }

    static void Rule_FunctionBodyTypeMismatch(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        const auto &sig = sym.GetFunction();
        if (!sig.bodyAnalysis)
        {
            return;
        }
        const auto &body = *sig.bodyAnalysis;

        std::unordered_map<std::string, std::string> scopeTypes;
        for (const auto &param : sig.parameters)
        {
            if (!param.name.empty() && !param.baseTypeName.empty())
            {
                scopeTypes[param.name] = param.baseTypeName;
            }
        }

        for (const auto &varInfo : body.bodyVariableTypes)
        {
            if (!varInfo.initializerText.empty() && !varInfo.typeName.empty())
            {
                std::string rhsName = varInfo.initializerText;
                while (!rhsName.empty() && (rhsName.back() == ';' || isspace(static_cast<unsigned char>(rhsName.back()))))
                {
                    rhsName.pop_back();
                }

                auto it = scopeTypes.find(rhsName);
                if (it != scopeTypes.end())
                {
                    const std::string &rhsType = it->second;
                    const std::string &lhsType = varInfo.typeName;

                    if (IsPrimitiveTypeName(lhsType) && IsPrimitiveTypeName(rhsType))
                    {
                        if ((lhsType == "bool" && rhsType != "bool") || (lhsType != "bool" && rhsType == "bool"))
                        {
                            ctx.LogRule("Rule_FunctionBodyTypeMismatch", "as-err-implicit-conversion", sym);
                            ctx.EmitAtRange(sym, varInfo.range, "as-err-implicit-conversion", rhsType);
                        }
                    }
                }
            }

            if (!varInfo.varName.empty() && !varInfo.typeName.empty())
            {
                scopeTypes[varInfo.varName] = varInfo.typeName;
            }
        }
    }

    void ValidateFunctionBody(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx)
    {
        Rule_FunctionBody(sym, fctx, ctx);
        Rule_FunctionBodyFlow(sym, fctx, ctx);
        Rule_FunctionBodySwitch(sym, fctx, ctx);
        Rule_FunctionBodyCast(sym, fctx, ctx);
        Rule_FunctionBodyVariables(sym, fctx, ctx);
        Rule_FunctionBodyScope(sym, fctx, ctx);
        Rule_FunctionReturnExpr(sym, fctx, ctx);
        Rule_FunctionBodyUnresolved(sym, fctx, ctx);
        Rule_FunctionBodyTypeMismatch(sym, fctx, ctx);
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

    static void ValidateParametersInternal(const Symbol &sym, const std::vector<ParameterInformation> &parameters, bool isExternalFunc, const DiagnosticContext &ctx)
    {
        std::string_view stringTypeName = ctx.request.GetStringTypeName();
        std::string_view arrayTypeName = ctx.request.GetArrayTypeName();

        ankerl::unordered_dense::set<std::string> seenParamNames;
        bool seenDefault = false;

        for (const auto &param : parameters)
        {
            if (isExternalFunc && param.modifier == ParameterModifier::InOut)
            {
                ctx.LogParam("ValidateFunctionParameters", "as-syntax-error", param, sym);
                ctx.Emit(param, sym, "as-syntax-error");
            }

            if (IsMixinClass(param.baseTypeName, ctx.request.symbolTable))
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-mixin-not-a-type", param, sym);
                ctx.Emit(param, sym, "as-err-mixin-not-a-type", param.name, sym.name);
            }

            if (isExternalFunc && !param.baseTypeName.empty() && !IsKnownType(param.baseTypeName, ctx) && !param.baseTypeName.starts_with("array<"))
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
                bool isUnnamedVoid = (parameters.size() == 1 && param.name.empty());
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
                    auto globalSyms = ctx.request.symbolTable.FindSymbolsPtr(param.name);
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

            if (!param.baseTypeName.empty() && !IsKnownType(param.baseTypeName, ctx))
            {
                ctx.LogParam("ValidateFunctionParameters", "as-err-unresolved-type", param, sym);
                ctx.Emit(param, sym, "as-err-unresolved-type", param.baseTypeName);
            }

            if (!param.isHandle)
            {
                auto typeSyms = ctx.request.symbolTable.FindSymbolsPtr(param.baseTypeName);
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

    void ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const DiagnosticContext &ctx)
    {
        ValidateParametersInternal(sym, sig.parameters, sig.modifiers.isExternal, ctx);
    }

    void ValidateFunctionParameters(const Symbol &sym, const FuncdefSignature &sig, const DiagnosticContext &ctx)
    {
        ValidateParametersInternal(sym, sig.parameters, sig.modifiers.isExternal, ctx);
    }
}
