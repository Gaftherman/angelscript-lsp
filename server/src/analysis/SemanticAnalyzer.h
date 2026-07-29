#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/Diagnostics.h"
#include "utils/LspLogger.h"

#include <string>
#include <vector>

namespace angel_lsp::analysis
{
    /** @brief Context request for semantic validation (Pass 2). */
    struct SemanticAnalysisRequest
    {
        const SymbolTable &symbolTable;
        std::string fileUri;                 // Optional URI filter; if empty, validates all symbols in the table.
        std::string predefinedFileExtension; // Configured predefined file extension (e.g. "as.predefined")
    };

    /**
     * @brief Performs semantic validation (Pass 2) on top-level symbols in the SymbolTable.
     *
     * Validates class hierarchy, mixin constraints, interface implementation, duplicates,
     * virtual properties, typedefs, funcdefs, enums, namespaces, and function parameter/return modifiers.
     */
    class SemanticAnalyzer
    {
    public:
        explicit SemanticAnalyzer(angel_lsp::utils::LspLogger *logger = nullptr);
        ~SemanticAnalyzer() = default;

        /**
         * @brief Validates the SymbolTable and returns all discovered diagnostics.
         * @param request Immutable context containing the SymbolTable reference.
         * @return Vector of Diagnostic objects ready for publishing.
         */
        std::vector<Diagnostic> Analyze(const SemanticAnalysisRequest &request) const;

    private:
        angel_lsp::utils::LspLogger *m_logger;

        // ─── Individual Category Validation Stubs ──────────────────────────────────

        /** @brief Validates class hierarchy, base class resolution, and circular inheritance. */
        void ValidateClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates mixin class constraints (e.g. cannot be final or abstract). */
        void ValidateMixinClass(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates interface declaration and inheritance. */
        void ValidateInterface(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates function signature, return type, return reference (int& Func()), and overloads. */
        void ValidateFunction(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates parameter modifiers (&in, &out, &inout), handle types (@), and default values. */
        void ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates global / member variable declarations and type modifiers. */
        void ValidateVariable(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates virtual property getter/setter declarations. */
        void ValidateVirtualProperty(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates enum declaration and top-level name collisions. */
        void ValidateEnum(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates typedef alias declarations. */
        void ValidateTypedef(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates funcdef callback delegate declarations. */
        void ValidateFuncdef(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates namespace declarations. */
        void ValidateNamespace(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const;

        /** @brief Validates duplicate symbol redeclarations within the same scope. */
        void ValidateDuplicateSymbols(const std::string &qualifiedName, const std::vector<Symbol> &symbols, std::vector<Diagnostic> &diagnostics) const;

        // ─── Helper Methods ────────────────────────────────────────────────────────

        /** @brief Constructs a Diagnostic object from a symbol and message. */
        Diagnostic CreateDiagnostic(const Symbol &sym, const std::string &message, const std::string &code, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
    };
}
