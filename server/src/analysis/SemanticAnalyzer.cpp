#include "analysis/SemanticAnalyzer.h"
#include "utils/Utils.h"
#include "spdlog/fmt/fmt.h"

#include <unordered_set>

namespace angel_lsp::analysis
{
    SemanticAnalyzer::SemanticAnalyzer(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
    }

    std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest &request) const
    {
        std::vector<Diagnostic> diagnostics;

        request.symbolTable.ForEachSymbol([&](const std::string &qualifiedName, const std::vector<Symbol> &symbols)
                                          {
            ValidateDuplicateSymbols(qualifiedName, symbols, request, diagnostics);

            for (const auto &sym : symbols)
            {
                if (!request.fileUri.empty() && sym.fileUri != request.fileUri)
                {
                    continue;
                }

                switch (sym.type)
                {
                case SymbolType::Class:
                {
                    if (sym.classSignature.modifiers.isMixin)
                    {
                        ValidateMixinClass(sym, request, diagnostics);
                    }
                    else
                    {
                        ValidateClass(sym, request, diagnostics);
                    }
                    break;
                }
                case SymbolType::Interface:
                {
                    ValidateInterface(sym, request, diagnostics);
                    break;
                }
                case SymbolType::Function:
                {
                    ValidateFunction(sym, request, diagnostics);
                    ValidateFunctionParameters(sym, sym.functionSignature, request, diagnostics);
                    break;
                }
                case SymbolType::Variable:
                {
                    ValidateVariable(sym, request, diagnostics);
                    break;
                }
                case SymbolType::Property:
                {
                    ValidateVirtualProperty(sym, request, diagnostics);
                    break;
                }
                case SymbolType::Enum:
                {
                    ValidateEnum(sym, request, diagnostics);
                    break;
                }
                case SymbolType::Typedef:
                {
                    ValidateTypedef(sym, request, diagnostics);
                    break;
                }
                case SymbolType::Funcdef:
                {
                    ValidateFuncdef(sym, request, diagnostics);
                    break;
                }
                case SymbolType::Namespace:
                {
                    ValidateNamespace(sym, request, diagnostics);
                    break;
                }
                default:
                    break;
                }
            } });

        if (m_logger && !diagnostics.empty())
        {
            m_logger->LogInfo(fmt::format("[SemanticAnalyzer] Se encontraron {} diagnósticos.", diagnostics.size()));
        }

        return diagnostics;
    }

    void SemanticAnalyzer::ValidateClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (sym.classSignature.isTemplate && !angel_lsp::utils::IsPredefinedFile(sym.fileUri, req.predefinedFileExtension))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-template-class-not-supported", sym.name));
        }

        uint32_t classBaseCount = 0;
        for (const auto &baseName : sym.classSignature.bases)
        {
            const auto *baseSymbols = req.symbolTable.FindSymbolsPtr(baseName);
            if (!baseSymbols || baseSymbols->empty())
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-base-not-found", baseName));
                continue;
            }

            for (const auto &baseSym : *baseSymbols)
            {
                if (baseSym.type == SymbolType::Class)
                {
                    classBaseCount++;
                    if (baseSym.classSignature.modifiers.isFinal)
                    {
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-inherit-final", baseName));
                    }
                }
            }
        }

        if (classBaseCount > 1)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-multi-class-inherit", sym.name));
        }
    }

    void SemanticAnalyzer::ValidateMixinClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (sym.classSignature.modifiers.isFinal)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-final", sym.name));
        }

        if (sym.classSignature.modifiers.isAbstract)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-mixin-abstract", sym.name));
        }
    }

    void SemanticAnalyzer::ValidateInterface(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (sym.classSignature.modifiers.isFinal)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-interface-final", sym.name));
        }

        if (sym.classSignature.modifiers.isAbstract)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-interface-abstract", sym.name));
        }
    }

    static bool IsPrimitiveTypeName(const std::string &name)
    {
        return name == "void" || name == "int" || name == "int8" || name == "int16" || name == "int32" || name == "int64"
            || name == "uint" || name == "uint8" || name == "uint16" || name == "uint32" || name == "uint64"
            || name == "float" || name == "double" || name == "bool";
    }

    static bool AreParametersEqual(const ParameterInformation &p1, const ParameterInformation &p2)
    {
        if (p1.baseTypeName != p2.baseTypeName || p1.templateName != p2.templateName || p1.isArray != p2.isArray || p1.modifier != p2.modifier)
        {
            return false;
        }

        if (IsPrimitiveTypeName(p1.baseTypeName))
        {
            return true;
        }

        return p1.isHandle == p2.isHandle;
    }

    static bool AreFunctionsDuplicate(const FunctionSignature &s1, const FunctionSignature &s2)
    {
        if (s1.parameters.size() != s2.parameters.size())
            return false;

        for (size_t i = 0; i < s1.parameters.size(); ++i)
        {
            if (!AreParametersEqual(s1.parameters[i], s2.parameters[i]))
                return false;
        }
        return true;
    }

    void SemanticAnalyzer::ValidateFunction(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (!sym.functionSignature.hasBody && !angel_lsp::utils::IsPredefinedFile(sym.fileUri, req.predefinedFileExtension))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-missing-body", sym.name));
        }

        if (!IsTypeResolvable(sym.functionSignature.returnBaseTypeName, sym.functionSignature.returnTemplateName, sym.functionSignature.returnTypeKind, req))
        {
            std::string unresolved = !sym.functionSignature.returnTemplateName.empty() && sym.functionSignature.returnTemplateName != "array" && !req.symbolTable.HasSymbol(sym.functionSignature.returnTemplateName)
                ? sym.functionSignature.returnTemplateName
                : sym.functionSignature.returnBaseTypeName;
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", unresolved));
        }
        else if (!sym.functionSignature.modifiers.isHandle && !sym.functionSignature.returnIsArray && IsFuncdefType(sym.functionSignature.returnBaseTypeName, req))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-funcdef-not-handle", sym.functionSignature.returnBaseTypeName));
        }
    }

    void SemanticAnalyzer::ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        for (const auto &param : sig.parameters)
        {
            if (param.modifier == ParameterModifier::Out && param.defaultValue.size() > 0)
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-out-param-default", param.name));
            }

            if (param.hasPrimitiveHandle || (IsPrimitiveTypeName(param.baseTypeName) && param.isHandle && !param.isArray))
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-handle-on-primitive", param.baseTypeName));
            }
            
            if (!IsTypeResolvable(param.baseTypeName, param.templateName, param.typeKind, req))
            {
                std::string unresolved = !param.templateName.empty() && param.templateName != "array" && !req.symbolTable.HasSymbol(param.templateName)
                    ? param.templateName
                    : param.baseTypeName;
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-unresolved-type", unresolved));
            }
            else if (!param.isHandle && !param.isArray && IsFuncdefType(param.baseTypeName, req))
            {
                diagnostics.push_back(CreateDiagnostic(param, sym, req, "as-err-funcdef-not-handle", param.baseTypeName));
            }
        }
    }

    void SemanticAnalyzer::ValidateVariable(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        const auto &sig = sym.variableSignature;

        if (sig.typeKind == TypeKind::Void)
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-void-variable"));
            return;
        }

        if (sig.hasPrimitiveHandle || (IsPrimitiveTypeName(sig.baseTypeName) && sig.modifiers.isHandle && !sig.isArray))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-handle-on-primitive", sig.baseTypeName));
            return;
        }

        if (!IsTypeResolvable(sig.baseTypeName, sig.templateName, sig.typeKind, req))
        {
            std::string unresolved = !sig.templateName.empty() && sig.templateName != "array" && !req.symbolTable.HasSymbol(sig.templateName)
                ? sig.templateName
                : sig.baseTypeName;
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", unresolved));
            return;
        }

        if (!sig.modifiers.isHandle && !sig.isArray && IsFuncdefType(sig.baseTypeName, req))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-funcdef-not-handle", sig.baseTypeName));
        }
    }

    void SemanticAnalyzer::ValidateVirtualProperty(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
    }

    void SemanticAnalyzer::ValidateEnum(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
    }

    void SemanticAnalyzer::ValidateTypedef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (!IsTypeResolvable(sym.typedefSignature.baseType, "", sym.typedefSignature.typeKind, req))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-typedef-unresolved", sym.typedefSignature.baseType));
        }
    }

    void SemanticAnalyzer::ValidateFuncdef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (!IsTypeResolvable(sym.functionSignature.returnBaseTypeName, sym.functionSignature.returnTemplateName, sym.functionSignature.returnTypeKind, req))
        {
            std::string unresolved = !sym.functionSignature.returnTemplateName.empty() && sym.functionSignature.returnTemplateName != "array" && !req.symbolTable.HasSymbol(sym.functionSignature.returnTemplateName)
                ? sym.functionSignature.returnTemplateName
                : sym.functionSignature.returnBaseTypeName;
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", unresolved));
        }

        for (const auto &param : sym.functionSignature.parameters)
        {
            if (!IsTypeResolvable(param.baseTypeName, param.templateName, param.typeKind, req))
            {
                std::string unresolved = !param.templateName.empty() && param.templateName != "array" && !req.symbolTable.HasSymbol(param.templateName)
                    ? param.templateName
                    : param.baseTypeName;
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-unresolved-type", unresolved));
            }
        }
    }

    void SemanticAnalyzer::ValidateNamespace(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
    }

    void SemanticAnalyzer::ValidateDuplicateSymbols(const std::string &qualifiedName, const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (symbols.size() <= 1)
            return;

        if (symbols.front().type == SymbolType::Namespace)
            return;

        if (symbols.front().type == SymbolType::Function)
        {
            for (size_t i = 0; i < symbols.size(); ++i)
            {
                for (size_t j = i + 1; j < symbols.size(); ++j)
                {
                    if (AreFunctionsDuplicate(symbols[i].functionSignature, symbols[j].functionSignature))
                    {
                        diagnostics.push_back(CreateDiagnostic(symbols[j], req, "as-err-duplicate-symbol", qualifiedName));
                    }
                }
            }
            return;
        }

        for (size_t i = 1; i < symbols.size(); ++i)
        {
            diagnostics.push_back(CreateDiagnostic(symbols[i], req, "as-err-duplicate-symbol", qualifiedName));
        }
    }

    bool SemanticAnalyzer::IsTypeResolvable(const std::string &baseTypeName, const std::string &templateName, TypeKind kind, const SemanticAnalysisRequest &req) const
    {
        if (!templateName.empty())
        {
            if (templateName != "array" && !req.symbolTable.HasSymbol(templateName))
            {
                return false;
            }
        }

        if (baseTypeName.empty())
            return true;

        if (IsPrimitiveTypeName(baseTypeName))
            return true;

        return req.symbolTable.HasSymbol(baseTypeName);
    }

    bool SemanticAnalyzer::IsFuncdefType(const std::string &baseTypeName, const SemanticAnalysisRequest &req) const
    {
        if (baseTypeName.empty())
            return false;

        const auto *syms = req.symbolTable.FindSymbolsPtr(baseTypeName);
        if (syms && !syms->empty())
        {
            return syms->front().type == SymbolType::Funcdef;
        }
        return false;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code) const
    {
        std::string message = req.i18n ? req.i18n->GetMessage(code) : "";

        Diagnostic diag;
        diag.range.start.line = sym.startLine;
        diag.range.start.character = sym.startCharacter;
        diag.range.end.line = sym.endLine;
        diag.range.end.character = sym.endCharacter;
        diag.severity = DiagnosticSeverity::Error;
        diag.code = code;
        diag.source = "AngelScript";
        diag.message = message;
        diag.fileUri = sym.fileUri;
        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1) const
    {
        std::string pattern = req.i18n ? req.i18n->GetMessage(code) : "";
        std::string message = fmt::format(fmt::runtime(pattern), arg1);

        Diagnostic diag;
        diag.range.start.line = sym.startLine;
        diag.range.start.character = sym.startCharacter;
        diag.range.end.line = sym.endLine;
        diag.range.end.character = sym.endCharacter;
        diag.severity = DiagnosticSeverity::Error;
        diag.code = code;
        diag.source = "AngelScript";
        diag.message = message;
        diag.fileUri = sym.fileUri;
        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code) const
    {
        std::string message = req.i18n ? req.i18n->GetMessage(code) : "";

        Diagnostic diag;
        diag.range.start.line = param.startLine;
        diag.range.start.character = param.startCharacter;
        diag.range.end.line = param.endLine;
        diag.range.end.character = param.endCharacter;
        diag.severity = DiagnosticSeverity::Error;
        diag.code = code;
        diag.source = "AngelScript";
        diag.message = message;
        diag.fileUri = parentSym.fileUri;
        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1) const
    {
        std::string pattern = req.i18n ? req.i18n->GetMessage(code) : "";
        std::string message = fmt::format(fmt::runtime(pattern), arg1);

        Diagnostic diag;
        diag.range.start.line = param.startLine;
        diag.range.start.character = param.startCharacter;
        diag.range.end.line = param.endLine;
        diag.range.end.character = param.endCharacter;
        diag.severity = DiagnosticSeverity::Error;
        diag.code = code;
        diag.source = "AngelScript";
        diag.message = message;
        diag.fileUri = parentSym.fileUri;
        return diag;
    }
}
