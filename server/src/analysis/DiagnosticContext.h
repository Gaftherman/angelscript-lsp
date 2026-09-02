#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/SymbolTable.h"
#include "analysis/SemanticAnalysisRequest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Forward-declared so diagnostic collection does not transitively depend on the LSP protocol library.
namespace angel_lsp::utils { class LspLogger; }

namespace angel_lsp::analysis
{
    /**
     * @brief Context for constructing, emitting, and logging LSP diagnostics during semantic analysis.
     */
    struct DiagnosticContext
    {
        const SemanticAnalysisRequest &request;
        std::vector<Diagnostic> &diagnostics;
        utils::LspLogger *logger = nullptr;

        // --- Diagnostic Emission for Symbol ---
        void Emit(const Symbol &sym, std::string_view code, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        void Emit(const Symbol &sym, std::string_view code, std::string_view arg1, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        void Emit(const Symbol &sym, std::string_view code, std::string_view arg1, std::string_view arg2, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        void Emit(const Symbol &sym, std::string_view code, std::string_view arg1, std::string_view arg2, std::string_view arg3, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;

        // --- Diagnostic Emission for ParameterInformation ---
        void Emit(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        void Emit(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, std::string_view arg1, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        void Emit(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, std::string_view arg1, std::string_view arg2, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;

        // --- Diagnostic Emission for SourceRange ---
        void EmitAtRange(const Symbol &parentSym, const SourceRange &range, std::string_view code, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        void EmitAtRange(const Symbol &parentSym, const SourceRange &range, std::string_view code, std::string_view arg1, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;

        // --- Diagnostic Emission for a raw range (no backing Symbol - e.g. a ScopeTree LocalReference).
        //     fileUri comes from request.fileUri instead of a Symbol's own fileUri. ---
        void EmitAtRange(uint32_t startLine, uint32_t startCharacter, uint32_t endLine, uint32_t endCharacter, std::string_view code, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        void EmitAtRange(uint32_t startLine, uint32_t startCharacter, uint32_t endLine, uint32_t endCharacter, std::string_view code, std::string_view arg1, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        void EmitAtRange(uint32_t startLine, uint32_t startCharacter, uint32_t endLine, uint32_t endCharacter, std::string_view code, std::string_view arg1, std::string_view arg2, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;

        // --- Debug Logging ---
        void LogRule(std::string_view ruleName, std::string_view code, const Symbol &sym) const;
        void LogParam(std::string_view ruleName, std::string_view code, const ParameterInformation &param, const Symbol &parentSym) const;

    private:
        Diagnostic CreateDiagnostic(const Symbol &sym, std::string_view code, DiagnosticSeverity severity) const;
        Diagnostic CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, DiagnosticSeverity severity) const;
    };
}
