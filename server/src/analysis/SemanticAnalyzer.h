#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"
#include "utils/LspLogger.h"

#include <string>
#include <vector>
#include <utility>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::analysis
{
    struct SemanticAnalysisRequest
    {
        const SymbolTable &symbolTable;
        std::string fileUri;
        std::string predefinedFileExtension;
        const i18n::I18n *i18n = nullptr;
    };

    class SemanticAnalyzer
    {
    public:
        explicit SemanticAnalyzer(angel_lsp::utils::LspLogger *logger = nullptr);
        ~SemanticAnalyzer() = default;

        std::vector<Diagnostic> Analyze(const SemanticAnalysisRequest &request) const;

    private:
        angel_lsp::utils::LspLogger *m_logger;

        void ValidateClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateMixinClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateInterface(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateFunction(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateVariable(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateVirtualProperty(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateEnum(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateTypedef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateFuncdef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateNamespace(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateDuplicateSymbols(const std::string &qualifiedName, const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        Diagnostic CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code) const;
        Diagnostic CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1) const;
        Diagnostic CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code) const;
        Diagnostic CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1) const;

        bool IsTypeResolvable(const std::string &baseTypeName, const std::string &templateName, TypeKind kind, const SemanticAnalysisRequest &req) const;
        bool IsFuncdefType(const std::string &baseTypeName, const SemanticAnalysisRequest &req) const;
    };
}
