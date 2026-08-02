#pragma once

#include "analysis/DiagnosticContext.h"

namespace angel_lsp::analysis::rules
{
    /**
     * @brief Context information about the enclosing scope of a function symbol.
     */
    struct FunctionContext
    {
        bool isInsideClass = false;
        bool isInsideMixin = false;
        bool isCtor = false;
        bool isDtor = false;
        bool isInterface = false;
        const Symbol *container = nullptr;
    };

    /**
     * @brief Constructs FunctionContext from a symbol and symbol table.
     * @param sym Function symbol to evaluate.
     * @param req Analysis request containing symbol table.
     * @return Initialized FunctionContext.
     */
    FunctionContext BuildFunctionContext(const Symbol &sym, const SemanticAnalysisRequest &req);

    /**
     * @brief Validates all semantic rules for a function symbol.
     * @param sym Function symbol to validate.
     * @param ctx Diagnostic context for error reporting.
     */
    void ValidateFunction(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief Validates parameter rules for a function signature.
     * @param sym Function parent symbol.
     * @param sig Function signature info.
     * @param ctx Diagnostic context.
     */
    void ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const DiagnosticContext &ctx);
}
