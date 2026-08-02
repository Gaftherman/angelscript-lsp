#pragma once

#include "analysis/DiagnosticContext.h"

namespace angel_lsp::analysis::rules
{
    /**
     * @brief Validates all semantic rules for a property symbol.
     * @param sym Property symbol to validate.
     * @param ctx Diagnostic context for error reporting.
     */
    void ValidateProperty(const Symbol &sym, const DiagnosticContext &ctx);
}
