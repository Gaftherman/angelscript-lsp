#pragma once

#include "analysis/DiagnosticContext.h"

namespace angel_lsp::analysis::rules
{
    /**
     * @brief Validates all semantic rules for a variable symbol.
     * @param sym Variable symbol to validate.
     * @param ctx Diagnostic context for error reporting.
     */
    void ValidateVariable(const Symbol &sym, const DiagnosticContext &ctx);
}
