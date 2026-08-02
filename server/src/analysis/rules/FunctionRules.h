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
     * @brief Validates function signature rules (OUTSIDE THE BODY: name, return type, modifiers, ctor/dtor, override, operator overloads, mixin constraints).
     * @param sym Function symbol to validate.
     * @param fctx Function context info.
     * @param ctx Diagnostic context for error reporting.
     * @return True if function signature is valid, false if execution should halt early.
     */
    bool ValidateFunctionSignature(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx);

    /**
     * @brief Validates parameter rules for a function signature (OUTSIDE THE BODY).
     * @param sym Function parent symbol.
     * @param sig Function signature info.
     * @param ctx Diagnostic context.
     */
    void ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const DiagnosticContext &ctx);

    /**
     * @brief Validates function body rules (INSIDE THE BODY: statements, flow control, cast, local variables scope, return expressions).
     * @param sym Function symbol to validate.
     * @param fctx Function context info.
     * @param ctx Diagnostic context for error reporting.
     */
    void ValidateFunctionBody(const Symbol &sym, const FunctionContext &fctx, const DiagnosticContext &ctx);

    /**
     * @brief Orchestrator method that validates both signature (outside body) and body (inside body) rules.
     * @param sym Function symbol to validate.
     * @param ctx Diagnostic context for error reporting.
     */
    void ValidateFunction(const Symbol &sym, const DiagnosticContext &ctx);
}
