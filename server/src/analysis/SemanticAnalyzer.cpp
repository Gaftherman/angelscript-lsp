#include "analysis/SemanticAnalyzer.h"
#include <spdlog/fmt/fmt.h>
#include <sstream>

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

    SemanticAnalyzer::FunctionContext SemanticAnalyzer::BuildFunctionContext(const Symbol &sym, const SemanticAnalysisRequest &req) const
    {
        FunctionContext ctx;
        ctx.isCtor = (!sym.containerName.empty() && sym.name == sym.containerName);
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

    void SemanticAnalyzer::DebugDiag(const std::string &ruleName, const std::string &code, const Symbol &sym) const
    {
        if (!m_logger)
        {
            return;
        }

        m_logger->LogDebug(fmt::format("[SA-DEBUG] rule={:<35} code={:<35} sym={} container={}",
                                       ruleName, code, sym.name, sym.containerName));
    }

    void SemanticAnalyzer::DebugParamDiag(const std::string &ruleName, const std::string &code, const ParameterInformation &param, const Symbol &parentSym) const
    {
        if (!m_logger)
        {
            return;
        }

        m_logger->LogDebug(fmt::format("[SA-DEBUG] rule={:<35} code={:<35} param={} parent={}",
                                       ruleName, code, param.name, parentSym.name));
    }

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
                if (seenParamNames.contains(param.name))
                {
                    DebugParamDiag("ValidateFunctionParameters", "as-err-duplicate-param", param, sym);
                    diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-duplicate-param", param.name, sym.name));
                }
                else
                {
                    seenParamNames.insert(param.name);
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

            if (!param.name.empty())
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

    bool SemanticAnalyzer::Rule_PropertyModifiers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetVariable();

        if (!sym.containerName.empty() && (sig.isVirtualProperty || sig.hasGet || sig.hasSet))
        {
            const auto *containerSyms = req.symbolTable.FindSymbolsPtr(sym.containerName);
            if (containerSyms)
            {
                for (const auto &cSym : *containerSyms)
                {
                    if (cSym.type == SymbolType::Class && cSym.GetClass().modifiers.isMixin)
                    {
                        DebugDiag("Rule_PropertyModifiers", "as-err-mixin-virtual-property", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-virtual-property"));
                        return true;
                    }
                }
            }
        }

        if (sig.typeKind == TypeKind::Void)
        {
            DebugDiag("Rule_PropertyModifiers", "as-err-void-variable", sym);
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
                        DebugDiag("Rule_PropertyModifiers", "as-err-class-member-const", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-class-member-const", sym.name));
                        break;
                    }
                }
            }
        }

        if (sig.hasPrimitiveHandle)
        {
            DebugDiag("Rule_PropertyModifiers", "as-err-handle-on-primitive", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
        }

        if (sig.typeKind == TypeKind::Unknown && !sig.baseTypeName.empty())
        {
            if (!req.symbolTable.HasSymbol(sig.baseTypeName))
            {
                DebugDiag("Rule_PropertyModifiers", "as-err-unresolved-type", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", sig.baseTypeName));
            }
        }

        return false;
    }

    void SemanticAnalyzer::Rule_PropertyAccessors(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.GetVariable();

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
            DebugDiag("Rule_PropertyAccessors", "as-err-global-function-qualifiers", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-global-function-qualifiers", sym.name));
        }

        if (isInterfaceProperty && (sig.hasBodyGet || sig.hasBodySet))
        {
            DebugDiag("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (!isInterfaceProperty && ((sig.hasGet && !sig.hasBodyGet) || (sig.hasSet && !sig.hasBodySet)))
        {
            DebugDiag("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (sig.hasDuplicateGet || sig.hasDuplicateSet)
        {
            DebugDiag("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
        }

        if (isInterfaceProperty && (sig.isGetFinal || sig.isGetOverride || sig.isSetFinal || sig.isSetOverride))
        {
            DebugDiag("Rule_PropertyAccessors", "as-syntax-error", sym);
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
                DebugDiag("Rule_PropertyAccessors", "as-err-override-no-base", sym);
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-override-no-base", sym.name, sym.containerName));
            }
        }

        if (!isInterfaceProperty && sig.modifiers.isProperty && !sym.GetVariable().modifiers.isExternal)
        {
            const auto *funcSyms = req.symbolTable.FindSymbolsPtr(sym.name);
            if (funcSyms)
            {
                for (const auto &fSym : *funcSyms)
                {
                    if (fSym.fileUri == sym.fileUri && fSym.startLine == sym.startLine &&
                        fSym.type == SymbolType::Function && !fSym.GetFunction().hasBody)
                    {
                        DebugDiag("Rule_PropertyAccessors", "as-err-property-accessor-missing-body", sym);
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-property-accessor-missing-body", sym.name));
                        break;
                    }
                }
            }
        }
    }

    void SemanticAnalyzer::ValidateProperty(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (Rule_PropertyModifiers(sym, req, diagnostics))
            return;

        Rule_PropertyAccessors(sym, req, diagnostics);
    }

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

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = sym.startLine;
        diag.range.start.character = sym.startCharacter;
        diag.range.end.line = sym.endLine;
        diag.range.end.character = sym.endCharacter;
        diag.severity = severity;
        if (req.severityOverrides)
        {
            auto it = req.severityOverrides->find(code);
            if (it != req.severityOverrides->end())
            {
                diag.severity = it->second;
            }
        }
        diag.code = code;
        diag.source = "AngelScript";
        diag.fileUri = sym.fileUri;

        if (req.i18n)
        {
            diag.message = req.i18n->GetMessage(code);
        }
        if (diag.message.empty())
        {
            diag.message = "[" + code + "] Diagnostic code: " + code;
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
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1;
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
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1 + ", " + arg2;
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
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1 + ", " + arg2 + ", " + arg3;
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
        if (req.severityOverrides)
        {
            auto it = req.severityOverrides->find(code);
            if (it != req.severityOverrides->end())
            {
                diag.severity = it->second;
            }
        }
        diag.code = code;
        diag.source = "AngelScript";
        diag.fileUri = parentSym.fileUri;

        if (req.i18n)
        {
            diag.message = req.i18n->GetMessage(code);
        }
        if (diag.message.empty())
        {
            diag.message = "[" + code + "] Diagnostic code: " + code;
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
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1;
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
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1 + ", " + arg2;
        }

        return diag;
    }
}
