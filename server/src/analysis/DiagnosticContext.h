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

        /**
         * @brief Emits at the type name inside a declaration, rather than over the declaration.
         *
         * `void process(BadType param)` underlined `BadType param`, and
         * `MissingType getThing() { ... }` underlined the whole signature. Both are the symbol's own
         * range, which is right for a rule about the declaration and wrong for one about the type
         * written in it: the reader is shown where to look, and the place to look is the type.
         *
         * The name is located in the source between the declaration's start and the end of that
         * line, which is what keeps `const BadType &in p` pointing at `BadType` rather than at the
         * first characters of `const`. A name that cannot be found there - a declaration split
         * across lines, or a type spelled differently from its base name - falls back to the
         * symbol's range, which is what this replaced and never worse than it.
         */
        void EmitAtTypeName(const Symbol &sym, std::string_view code, std::string_view typeName,
                            DiagnosticSeverity severity = DiagnosticSeverity::Error) const;

        /** @brief The same, for a type written inside one parameter. */
        void EmitAtTypeName(const ParameterInformation &param, const Symbol &parentSym,
                            std::string_view code, std::string_view typeName,
                            DiagnosticSeverity severity = DiagnosticSeverity::Error) const;

        // --- Debug Logging ---
        void LogRule(std::string_view ruleName, std::string_view code, const Symbol &sym) const;
        void LogParam(std::string_view ruleName, std::string_view code, const ParameterInformation &param, const Symbol &parentSym) const;

    private:
        /**
         * @brief Appends a finished diagnostic, unless the engine's warning mode says to drop it.
         *
         * asEP_COMPILER_WARNINGS is the one engine property that moves a severity rather than
         * deciding whether something is legal, so it belongs here rather than in any single rule.
         * Measured on a script producing "Signed/Unsigned mismatch": 0 suppresses the warning
         * entirely, 1 emits it, 2 turns it into an error.
         *
         * Every Emit overload routes through this, which is the point - twelve push_back sites
         * would otherwise each have had to remember.
         */
        void Append(Diagnostic &&diag) const;

        Diagnostic CreateDiagnostic(const Symbol &sym, std::string_view code, DiagnosticSeverity severity) const;
        Diagnostic CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, DiagnosticSeverity severity) const;
    };
}
