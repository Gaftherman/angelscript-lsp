#include "analysis/SemanticAnalyzer.h"
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::analysis
{
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

        if (firstType != SymbolType::Function && firstType != SymbolType::Funcdef)
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

        if (firstType == SymbolType::Function)
        {
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
                        if (sigI.parameters[p].baseTypeName != sigJ.parameters[p].baseTypeName)
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
        const auto &sig = sym.GetFunction();

        if (!sig.hasBody && !sig.isInterfaceMethod && !sig.modifiers.isExternal && !sig.modifiers.isDelete)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-missing-body", sym.name));
        }

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

    void SemanticAnalyzer::ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        ankerl::unordered_dense::set<std::string> seenParamNames;

        for (const auto &param : sig.parameters)
        {
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

            if (param.modifier == ParameterModifier::Out && !param.defaultValue.empty())
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
    }

    void SemanticAnalyzer::ValidateClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
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
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-base-not-found", baseName));
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
                            break;
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
