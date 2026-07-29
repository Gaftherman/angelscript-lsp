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

        for (const auto &baseName : sym.classSignature.bases)
        {
            const auto *baseSymbols = req.symbolTable.FindSymbolsPtr(baseName);
            if (baseSymbols && !baseSymbols->empty())
            {
                for (const auto &baseSym : *baseSymbols)
                {
                    if (baseSym.type == SymbolType::Class && baseSym.classSignature.modifiers.isFinal)
                    {
                        diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-inherit-final", baseName));
                    }
                }
            }
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
    }

    void SemanticAnalyzer::ValidateFunction(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        if (!sym.functionSignature.hasBody && !angel_lsp::utils::IsPredefinedFile(sym.fileUri, req.predefinedFileExtension))
        {
            diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-missing-body", sym.name));
        }
    }

    void SemanticAnalyzer::ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
        for (const auto &param : sig.parameters)
        {
            if (param.modifier == ParameterModifier::Out && param.defaultValue.size() > 0)
            {
                diagnostics.push_back(CreateDiagnostic(sym, req, "as-err-out-param-default", param.name));
            }
        }
    }

    void SemanticAnalyzer::ValidateVariable(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
    }

    void SemanticAnalyzer::ValidateVirtualProperty(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
    }

    void SemanticAnalyzer::ValidateEnum(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
    }

    void SemanticAnalyzer::ValidateTypedef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
    }

    void SemanticAnalyzer::ValidateFuncdef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const
    {
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
            return;
        }

        for (size_t i = 1; i < symbols.size(); ++i)
        {
            diagnostics.push_back(CreateDiagnostic(symbols[i], req, "as-err-duplicate-symbol", qualifiedName));
        }
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
}
