#pragma once

#include "analysis/DiagnosticContext.h"

namespace angel_lsp::analysis::rules
{
    /**
     * @brief Validates a function, method, constructor or destructor declaration.
     *
     * Declaration level only: the shape of the signature, the modifiers it carries and where it
     * sits. Nothing here reads the body - what a body does is the control-flow checker's work, and
     * what an expression inside it means is the conversion checker's.
     *
     * @param sym Function symbol to validate.
     * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
     */
    void ValidateFunction(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief Validates a parameter list on its own.
     *
     * Split out because a funcdef carries the same ParameterInformation as a function and the same
     * rules apply to it, minus everything that presumes a body or a container.
     *
     * @param sym Symbol owning the list, used for the diagnostic's message and file.
     * @param parameters The list to validate.
     * @param isFuncdef True when the owner is a funcdef, which admits a bodiless declaration.
     */
    void ValidateParameters(const Symbol &sym,
                            const std::vector<ParameterInformation> &parameters,
                            bool isFuncdef,
                            const DiagnosticContext &ctx);
}
