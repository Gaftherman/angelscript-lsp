#include "analysis/SemanticAnalyzerInternal.h"

namespace angel_lsp::analysis
{
    bool SemanticAnalyzer::Rule_FunctionName(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();

        if (sym.containerName.empty() && (sym.name == arrayTypeName || sym.name == stringTypeName))
        {
            DebugDiag("Rule_FunctionName", "as-err-name-conflict", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-name-conflict", sym.name, "registered object type"));
            return false;
        }

        if (IsReservedKeyword(sym.name))
        {
            DebugDiag("Rule_FunctionName", "as-err-reserved-keyword-name", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-reserved-keyword-name", sym.name));
            return false;
        }

        return true;
    }

    void SemanticAnalyzer::Rule_FunctionBody(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetFunction();

        if (!sig.hasBody && !sig.isInterfaceMethod && !sig.modifiers.isExternal && !sig.modifiers.isDelete)
        {
            DebugDiag("Rule_FunctionBody", "as-err-missing-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-missing-body", sym.name));
        }

        if ((sig.modifiers.isDelete || sig.modifiers.isExternal) && sig.hasBody)
        {
            DebugDiag("Rule_FunctionBody", "as-err-delete-with-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-delete-with-body", sym.name));
        }

        if (!sym.containerName.empty() && sig.returnType.empty() && sym.name != sym.containerName && sym.name[0] != '~')
        {
            DebugDiag("Rule_FunctionBody", "as-err-missing-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-missing-body", sym.name));
        }
    }

    void SemanticAnalyzer::Rule_FunctionReturnType(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();
        const auto &sig = sym.GetFunction();

        if ((sig.returnHasPrimitiveHandle && !sig.returnIsArray && sig.returnType.find("[]") == std::string::npos && sig.returnType.find("array<") == std::string::npos) || (sig.modifiers.isHandle && sig.returnBaseTypeName == stringTypeName && !sig.returnIsArray))
        {
            DebugDiag("Rule_FunctionReturnType", "as-err-handle-on-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }

        if (sig.returnBaseTypeName == "auto")
        {
            DebugDiag("Rule_FunctionReturnType", "as-err-unresolved-type", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", "auto"));
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.returnIsConst)
        {
            DebugDiag("Rule_FunctionReturnType", "as-err-const-void-return", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-const-void-return"));
        }

        if (sig.modifiers.isExternal && !sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" && !IsPrimitiveTypeName(sig.returnBaseTypeName) && sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName && !req.symbolTable.HasSymbolAnywhere(sig.returnBaseTypeName))
        {
            DebugDiag("Rule_FunctionReturnType", "as-err-unresolved-type", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.returnBaseTypeName));
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.modifiers.isReturnReference)
        {
            DebugDiag("Rule_FunctionReturnType", "as-err-void-reference", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-reference"));
        }

        if (sig.returnTypeKind == TypeKind::Void && sig.hasValueReturn)
        {
            DebugDiag("Rule_FunctionReturnType", "as-syntax-error", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
        }

        if (IsPrimitiveTypeName(sig.returnBaseTypeName) && !sig.returnIsArray && sig.returnTypeKind != TypeKind::Array && sig.returnType.find("[]") == std::string::npos && sig.returnType.find("array<") == std::string::npos && sig.returnExpression == "null")
        {
            DebugDiag("Rule_FunctionReturnType", "as-err-handle-on-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }

        if (sig.returnBaseTypeName != "void" && sig.returnTypeKind != TypeKind::Void && !sig.returnBaseTypeName.empty() && !ctx.isCtor && (!sym.name.empty() && sym.name[0] != '~'))
        {
            if (sig.hasBody && !sig.hasValueReturn && !sig.hasEmptyReturn)
            {
                DebugDiag("Rule_FunctionReturnType", "as-syntax-error", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }

        bool isReturnArray = sig.returnIsArray || sig.returnBaseTypeName == arrayTypeName;
        bool isArrayHandle = sig.modifiers.isHandle || sig.returnType.find("@") != std::string::npos;
        if (isReturnArray && !isArrayHandle && !sig.returnHasPrimitiveHandle && sig.hasNullReturn)
        {
            DebugDiag("Rule_FunctionReturnType", "as-err-handle-on-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.returnBaseTypeName));
        }

        if (!ctx.isInsideClass && sig.modifiers.isReturnReference && IsPrimitiveTypeName(sig.returnBaseTypeName) && !sig.modifiers.isConst && !sig.returnIsConst)
        {
            DebugDiag("Rule_FunctionReturnType", "as-err-invalid-reference-return", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-invalid-reference-return", sig.returnBaseTypeName));
        }

        bool isExternalFunc = sig.modifiers.isExternal;
        if ((sig.returnTypeKind == TypeKind::Unknown || isExternalFunc) && !sig.returnBaseTypeName.empty())
        {
            if (sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName &&
                !sig.returnBaseTypeName.starts_with("array<") &&
                !IsPrimitiveTypeName(sig.returnBaseTypeName) &&
                !req.symbolTable.HasSymbolAnywhere(sig.returnBaseTypeName))
            {
                DebugDiag("Rule_FunctionReturnType", "as-err-unresolved-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.returnBaseTypeName));
            }
        }
    }

    void SemanticAnalyzer::Rule_FunctionModifiers(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetFunction();

        if (sig.modifiers.isProperty)
        {
            if (!sym.name.starts_with("get_") && !sym.name.starts_with("set_"))
            {
                DebugDiag("Rule_FunctionModifiers", "as-syntax-error", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }

        if (!ctx.isInsideClass && (sig.modifiers.isConst || sig.modifiers.isOverride || sig.modifiers.isFinal || sig.modifiers.isExplicit || sig.modifiers.isDelete || sig.modifiers.isProperty || sig.modifiers.access == AccessModifier::Protected || sig.modifiers.access == AccessModifier::Private))
        {
            DebugDiag("Rule_FunctionModifiers", "as-err-global-function-qualifiers", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-function-qualifiers", sym.name));
        }

        if (sig.modifiers.isDelete && (sig.modifiers.isConst || (!ctx.isCtor && (sig.modifiers.isOverride || sig.modifiers.isFinal || sig.modifiers.isExplicit))))
        {
            DebugDiag("Rule_FunctionModifiers", "as-err-delete-with-other-qualifier", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-delete-with-other-qualifier", sym.name));
        }
    }

    void SemanticAnalyzer::Rule_CtorDtor(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetFunction();

        if (ctx.isDtor)
        {
            if (sig.modifiers.isShared || sig.modifiers.isExternal)
            {
                DebugDiag("Rule_CtorDtor", "as-syntax-error", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }

        if (ctx.isCtor || ctx.isDtor)
        {
            if (sig.modifiers.isFinal || sig.modifiers.isAbstract)
            {
                DebugDiag("Rule_CtorDtor", "as-syntax-error", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }

        if (sig.returnTypeKind != TypeKind::Void && sig.returnType != "void" && !ctx.isCtor && !ctx.isDtor)
        {
            if (sig.hasEmptyReturn)
            {
                DebugDiag("Rule_CtorDtor", "as-err-destructor-return-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-return-type", sym.name));
            }
        }

        if (ctx.isCtor && !sig.returnBaseTypeName.empty())
        {
            DebugDiag("Rule_CtorDtor", "as-err-destructor-return-type", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-return-type", sym.name));
        }

        if (ctx.isCtor || ctx.isDtor)
        {
            if (sig.hasValueReturn)
            {
                DebugDiag("Rule_CtorDtor", "as-err-destructor-return-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-return-type", sym.name));
            }
        }

        if (ctx.isDtor)
        {
            bool isUnnamedVoid = (sig.parameters.size() == 1 && (sig.parameters[0].baseTypeName == "void" || sig.parameters[0].typeKind == TypeKind::Void) && sig.parameters[0].name.empty());
            if (!sig.parameters.empty() && !isUnnamedVoid)
            {
                DebugDiag("Rule_CtorDtor", "as-err-destructor-param", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-param", sym.name));
            }
            if (!sig.returnType.empty())
            {
                DebugDiag("Rule_CtorDtor", "as-err-destructor-return-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-return-type", sym.name));
            }
            if (sig.modifiers.isDelete)
            {
                DebugDiag("Rule_CtorDtor", "as-err-destructor-delete", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-destructor-delete", sym.name));
            }
        }
    }

    void SemanticAnalyzer::Rule_FunctionBodyFlow(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetFunction();

        if (!sig.defaultValue.empty())
        {
            if ((sig.defaultValue.find("break;") != std::string::npos || sig.defaultValue.find("continue;") != std::string::npos) &&
                sig.defaultValue.find("while") == std::string::npos &&
                sig.defaultValue.find("for") == std::string::npos &&
                sig.defaultValue.find("switch") == std::string::npos)
            {
                DebugDiag("Rule_FunctionBodyFlow", "as-syntax-error", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }

            if (sig.defaultValue.find("case '") != std::string::npos)
            {
                DebugDiag("Rule_FunctionBodyFlow", "as-syntax-error", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }
    }

    void SemanticAnalyzer::Rule_FunctionBodyCast(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        const auto &sig = sym.GetFunction();

        if (!sig.defaultValue.empty())
        {
            size_t castPos = sig.defaultValue.find("cast<");
            while (castPos != std::string::npos)
            {
                size_t endAngle = sig.defaultValue.find('>', castPos + 5);
                if (endAngle != std::string::npos)
                {
                    std::string castTarget = sig.defaultValue.substr(castPos + 5, endAngle - (castPos + 5));
                    castTarget.erase(0, castTarget.find_first_not_of(" \t"));
                    castTarget.erase(castTarget.find_last_not_of(" \t") + 1);

                    if (castTarget == "void" || IsPrimitiveTypeName(castTarget) || castTarget == stringTypeName || (!req.symbolTable.HasSymbolAnywhere(castTarget) && !castTarget.empty()))
                    {
                        DebugDiag("Rule_FunctionBodyCast", "as-err-unresolved-type", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", "cast"));
                    }
                }
                castPos = sig.defaultValue.find("cast<", castPos + 5);
            }
        }
    }

    void SemanticAnalyzer::Rule_FunctionBodyScope(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetFunction();

        if (!sig.defaultValue.empty())
        {
            size_t scopePos = sig.defaultValue.find("::");
            while (scopePos != std::string::npos && scopePos > 0)
            {
                size_t startIdent = scopePos;
                while (startIdent > 0 && (isalnum(static_cast<unsigned char>(sig.defaultValue[startIdent - 1])) || sig.defaultValue[startIdent - 1] == '_'))
                {
                    startIdent--;
                }
                size_t endIdent = scopePos + 2;
                while (endIdent < sig.defaultValue.size() && (isalnum(static_cast<unsigned char>(sig.defaultValue[endIdent])) || sig.defaultValue[endIdent] == '_'))
                {
                    endIdent++;
                }
                std::string qualifiedName = sig.defaultValue.substr(startIdent, endIdent - startIdent);
                std::string scopePrefix = sig.defaultValue.substr(startIdent, scopePos - startIdent);

                if (!qualifiedName.empty() && scopePrefix != "global" && endIdent > scopePos + 2)
                {
                    if (!req.symbolTable.HasSymbolAnywhere(qualifiedName))
                    {
                        DebugDiag("Rule_FunctionBodyScope", "as-syntax-error", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
                        break;
                    }
                }
                scopePos = sig.defaultValue.find("::", endIdent);
            }
        }
    }

    void SemanticAnalyzer::Rule_FunctionReturnExpr(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();
        const auto &sig = sym.GetFunction();

        if (sig.hasValueReturn && IsReservedKeyword(sig.returnExpression) && sig.returnExpression != "null" && sig.returnExpression != "true" && sig.returnExpression != "false")
        {
            DebugDiag("Rule_FunctionReturnExpr", "as-syntax-error", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
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
                if (!req.symbolTable.HasSymbol(expectedQN) && !req.symbolTable.HasSymbol(target))
                {
                    DebugDiag("Rule_FunctionReturnExpr", "as-err-unresolved-type", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", target));
                }
            }
        }

        if (!sig.defaultValue.empty())
        {
            if (sig.defaultValue.find("super(") != std::string::npos || sig.defaultValue.find("super (") != std::string::npos)
            {
                if (!sym.containerName.empty())
                {
                    const auto *classSyms = req.symbolTable.FindSymbolsPtr(sym.containerName);
                    if (classSyms)
                    {
                        for (const auto &cSym : *classSyms)
                        {
                            if (cSym.type == SymbolType::Class && cSym.GetClass().bases.empty())
                            {
                                DebugDiag("Rule_FunctionReturnExpr", "as-syntax-error", sym);
                                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
                            }
                        }
                    }
                }
            }

            if (sig.modifiers.isExternal)
            {
                if (sig.defaultValue.find("inout") != std::string::npos || sig.defaultValue.find("&inout") != std::string::npos)
                {
                    DebugDiag("Rule_FunctionReturnExpr", "as-syntax-error", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
                }
                if (!sig.returnBaseTypeName.empty() && !IsPrimitiveTypeName(sig.returnBaseTypeName) && sig.returnBaseTypeName != stringTypeName && sig.returnBaseTypeName != arrayTypeName && !sig.returnBaseTypeName.starts_with("array<") && !req.symbolTable.HasSymbolAnywhere(sig.returnBaseTypeName))
                {
                    DebugDiag("Rule_FunctionReturnExpr", "as-err-unresolved-type", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.returnBaseTypeName));
                }
            }
        }

        if (sig.returnCallTargetName == "super" && ctx.isInsideClass)
        {
            auto parentOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && parentOpt->type == SymbolType::Class && parentOpt->GetClass().bases.empty())
            {
                DebugDiag("Rule_FunctionReturnExpr", "as-err-no-base-class", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-no-base-class", sym.name));
            }
        }
    }

    void SemanticAnalyzer::Rule_FunctionOverride(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetFunction();

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
                        DebugDiag("Rule_FunctionOverride", "as-err-global-function-qualifiers", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-function-qualifiers", sym.name));
                    }
                }
                else
                {
                    if (sig.modifiers.isOverride)
                    {
                        for (const auto &cSym : *containerSyms)
                        {
                            if (cSym.type == SymbolType::Class)
                            {
                                bool isCtorDtor = (sym.name == cSym.name || (!sym.name.empty() && sym.name[0] == '~'));
                                if (isCtorDtor)
                                {
                                    break;
                                }
                                if (cSym.GetClass().bases.empty())
                                {
                                    DebugDiag("Rule_FunctionOverride", "as-err-override-no-base", sym);
                                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-override-no-base", sym.name, sym.containerName));
                                }
                                else
                                {
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
                                        DebugDiag("Rule_FunctionOverride", "as-err-override-no-base", sym);
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
    }

    void SemanticAnalyzer::Rule_OperatorOverload(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetFunction();

        if (sym.name.rfind("op", 0) == 0 && !sym.containerName.empty())
        {
            if (sym.name == "opEquals")
            {
                if (sig.parameters.empty() || (sig.returnType != "bool" && sig.returnType != "int"))
                {
                    DebugDiag("Rule_OperatorOverload", "as-err-opequals-return-bool", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-opequals-return-bool", sym.name));
                }
            }
            else if (sym.name == "opCmp")
            {
                if (sig.returnType != "int" && sig.returnType != "bool")
                {
                    DebugDiag("Rule_OperatorOverload", "as-err-opcmp-return-int", sym);
                    diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-opcmp-return-int", sym.name));
                }
            }
        }
    }

    void SemanticAnalyzer::Rule_MixinConstraints(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetFunction();

        if (sig.modifiers.isDelete)
        {
            bool isMixinMember = false;
            if (!sym.containerName.empty())
            {
                auto pOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
                if (pOpt && pOpt->type == SymbolType::Class && pOpt->GetClass().modifiers.isMixin)
                {
                    isMixinMember = true;
                }
            }
            bool isAutoGeneratable = isMixinMember || (sym.name == "opAssign" || sym.name == "opEquals" || sym.name == "opCmp" || (!sym.containerName.empty() && (sym.name == sym.containerName || sym.name == "~" + sym.containerName)));
            if (!isAutoGeneratable)
            {
                DebugDiag("Rule_MixinConstraints", "as-syntax-error", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
            }
        }

        if (!sym.containerName.empty())
        {
            auto parentOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt && (parentOpt->type == SymbolType::Class || parentOpt->type == SymbolType::Interface))
            {
                if (parentOpt->type == SymbolType::Class && parentOpt->GetClass().modifiers.isMixin)
                {
                    bool isCtorCheck = (sym.name == sym.containerName);
                    bool isDtorCheck = (!sym.name.empty() && sym.name[0] == '~');
                    if (isCtorCheck || isDtorCheck || sig.modifiers.isDelete)
                    {
                        DebugDiag("Rule_MixinConstraints", "as-syntax-error", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-syntax-error"));
                    }
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateFunction(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const FunctionContext ctx = BuildFunctionContext(sym, req);

        if (!Rule_FunctionName(sym, ctx, req, diagnostics))
        {
            return;
        }

        Rule_FunctionBody(sym, ctx, req, diagnostics);
        Rule_FunctionReturnType(sym, ctx, req, diagnostics);
        Rule_FunctionModifiers(sym, ctx, req, diagnostics);
        Rule_CtorDtor(sym, ctx, req, diagnostics);
        Rule_FunctionBodyFlow(sym, ctx, req, diagnostics);
        Rule_FunctionBodyCast(sym, ctx, req, diagnostics);
        Rule_FunctionBodyScope(sym, ctx, req, diagnostics);
        Rule_FunctionReturnExpr(sym, ctx, req, diagnostics);
        Rule_FunctionOverride(sym, ctx, req, diagnostics);
        Rule_OperatorOverload(sym, ctx, req, diagnostics);
        Rule_MixinConstraints(sym, ctx, req, diagnostics);

        ValidateFunctionParameters(sym, sym.GetFunction(), req, diagnostics);
    }

    void SemanticAnalyzer::ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        std::string_view stringTypeName = req.GetStringTypeName();
        std::string_view arrayTypeName = req.GetArrayTypeName();

        ankerl::unordered_dense::set<std::string> seenParamNames;
        bool seenDefault = false;
        bool isExternalFunc = sig.modifiers.isExternal;

        for (const auto &param : sig.parameters)
        {
            if (isExternalFunc && param.modifier == ParameterModifier::InOut)
            {
                DebugParamDiag("ValidateFunctionParameters", "as-syntax-error", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-syntax-error"));
            }

            if (isExternalFunc && !param.baseTypeName.empty() && !IsPrimitiveTypeName(param.baseTypeName) && param.baseTypeName != stringTypeName && param.baseTypeName != arrayTypeName && !param.baseTypeName.starts_with("array<") && !req.symbolTable.HasSymbolAnywhere(param.baseTypeName))
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-unresolved-type", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-unresolved-type", param.baseTypeName));
            }

            if (!param.defaultValue.empty())
            {
                seenDefault = true;
            }
            else if (seenDefault && sym.type != SymbolType::Funcdef)
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-default-param-order", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-default-param-order", param.name, sym.name));
            }

            if (param.typeKind == TypeKind::Void || param.baseTypeName == "void")
            {
                bool isUnnamedVoid = (sig.parameters.size() == 1 && param.name.empty());
                if (!isUnnamedVoid && sym.type != SymbolType::Funcdef)
                {
                    DebugParamDiag("ValidateFunctionParameters", "as-err-void-parameter", param, sym);
                    diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-void-parameter", param.name, sym.name));
                }
            }

            if (param.baseTypeName == "auto")
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-unresolved-type", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-unresolved-type", "auto"));
            }

            if (param.modifier == ParameterModifier::InOut &&
                (param.isHandle || IsPrimitiveTypeName(param.baseTypeName) || param.baseTypeName == stringTypeName || param.typeKind == TypeKind::String || param.typeKind == TypeKind::Int32 || param.typeKind == TypeKind::Float || param.typeKind == TypeKind::Bool))
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-inout-on-primitive", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-inout-on-primitive", param.baseTypeName));
            }

            if (param.hasDoubleReference)
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-double-reference", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-double-reference", param.baseTypeName));
            }

            bool isStandaloneRef = param.isStandaloneRef;
            if (isStandaloneRef && (IsPrimitiveTypeName(param.baseTypeName) || param.typeKind == TypeKind::Int32 || param.typeKind == TypeKind::Float || param.typeKind == TypeKind::Bool || param.typeKind == TypeKind::Double || param.typeKind == TypeKind::UInt32 || param.baseTypeName == stringTypeName))
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-standalone-reference", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-standalone-reference", param.name));
            }
            else if (isStandaloneRef && !param.defaultValue.empty())
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-invalid-reference-return", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-invalid-reference-return", param.baseTypeName));
            }

            if (!param.name.empty())
            {
                if (IsReservedKeyword(param.name))
                {
                    DebugParamDiag("ValidateFunctionParameters", "as-err-reserved-keyword-name", param, sym);
                    diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-reserved-keyword-name", param.name));
                }
                if (!seenParamNames.insert(param.name).second)
                {
                    DebugParamDiag("ValidateFunctionParameters", "as-err-duplicate-param", param, sym);
                    diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-duplicate-param", param.name, sym.name));
                }
                else
                {
                    const auto *globalSyms = req.symbolTable.FindSymbolsPtr(param.name);
                    if (globalSyms)
                    {
                        for (const auto &gSym : *globalSyms)
                        {
                            if (gSym.containerName.empty() && gSym.type == SymbolType::Variable)
                            {
                                DebugParamDiag("ValidateFunctionParameters", "as-warn-shadow-global", param, sym);
                                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-warn-shadow-global", param.name, DiagnosticSeverity::Warning));
                                break;
                            }
                        }
                    }
                }
            }

            if (param.hasPrimitiveHandle || (param.baseTypeName == stringTypeName && param.isHandle))
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-handle-on-primitive", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-handle-on-primitive", param.baseTypeName));
            }

            if (!param.baseTypeName.empty() && !IsPrimitiveTypeName(param.baseTypeName) && param.baseTypeName != stringTypeName && param.baseTypeName != arrayTypeName && !req.symbolTable.HasSymbol(param.baseTypeName) && !req.symbolTable.HasSymbolAnywhere(param.baseTypeName))
            {
                DebugParamDiag("ValidateFunctionParameters", "as-err-unresolved-type", param, sym);
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-unresolved-type", param.baseTypeName));
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
                            DebugParamDiag("ValidateFunctionParameters", "as-err-funcdef-not-handle", param, sym);
                            diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-funcdef-not-handle", param.baseTypeName, param.baseTypeName));
                            break;
                        }
                    }
                }
            }
        }
    }
}
