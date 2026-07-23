/**
 * @file IValidationRule.h
 * @brief Interface for modular validation rules in the pipeline.
 * @ingroup Analysis
 */

#pragma once

#include <vector>
#include <string>
#include "ValidationContext.h"
#include "lsp/types.h"

namespace analysis::validation
{
    using lsPosition = lsp::Position;
    using lsRange = lsp::Range;
    using lsDiagnostic = lsp::Diagnostic;
    using lsDiagnosticSeverity = lsp::DiagnosticSeverity;

    /**
     * @brief Pure virtual interface for all pipeline validation rules.
     */
    class IValidationRule
    {
    public:
        virtual ~IValidationRule() = default;

        /**
         * @brief Returns a unique identifier string for debugging and tracing.
         */
        virtual std::string getName() const = 0;

        /**
         * @brief Executes the validation rule over the given context.
         * @param[in] ctx The validation context containing AST and symbol table references.
         * @return std::vector<lsDiagnostic> A collection of LSP diagnostics produced by this rule.
         */
        virtual std::vector<lsDiagnostic> run(const ValidationContext &ctx) = 0;
    };
}
