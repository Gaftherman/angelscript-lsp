#pragma once

#include "analysis/DiagnosticContext.h"

namespace angel_lsp::analysis::rules
{
    /**
     * @brief Validates a variable, field or virtual property declaration.
     *
     * Covers what the declared type allows - void, handles on primitives, funcdefs that must be
     * handles, mixins used as a type - and what the declaration's modifiers allow where it sits.
     * Virtual property accessors are validated here too, since they arrive as VariableSignature.
     *
     * @param sym Variable or property symbol to validate.
     * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
     */
    void ValidateVariable(const Symbol &sym, const DiagnosticContext &ctx);
}
