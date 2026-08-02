#pragma once

#include "analysis/DiagnosticContext.h"
#include <string>
#include <vector>

namespace angel_lsp::analysis::rules
{
    /**
     * @brief Validates duplicate symbol declarations in a given scope.
     * @param qualifiedName Qualified symbol name.
     * @param symbols Set of symbols declared under qualifiedName.
     * @param ctx Diagnostic context for error reporting.
     */
    void ValidateDuplicates(const std::string &qualifiedName, const std::vector<Symbol> &symbols, const DiagnosticContext &ctx);

    /**
     * @brief Validates semantic rules for an interface symbol.
     * @param sym Interface symbol.
     * @param ctx Diagnostic context.
     */
    void ValidateInterface(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief Validates semantic rules for a typedef symbol.
     * @param sym Typedef symbol.
     * @param ctx Diagnostic context.
     */
    void ValidateTypedef(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief Validates semantic rules for a funcdef symbol.
     * @param sym Funcdef symbol.
     * @param ctx Diagnostic context.
     */
    void ValidateFuncdef(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief Validates semantic rules for an enum symbol.
     * @param sym Enum symbol.
     * @param ctx Diagnostic context.
     */
    void ValidateEnum(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief Validates semantic rules for a namespace symbol.
     * @param sym Namespace symbol.
     * @param ctx Diagnostic context.
     */
    void ValidateNamespace(const Symbol &sym, const DiagnosticContext &ctx);
}
