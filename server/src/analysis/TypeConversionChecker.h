#pragma once

#include "analysis/DiagnosticContext.h"

#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Checks that every conversion written in a document has a declaration backing it.
     *
     * AngelScript reaches a target type through four routes, and each one is a declaration this
     * analyzer can see when the types involved are visible:
     *
     * | Written as        | Satisfied by                                  |
     * | ----------------- | --------------------------------------------- |
     * | `T v = expr;`     | copy/converting constructor, opImplConv, opAssign |
     * | `T(expr)`         | constructor, opConv, opImplConv                |
     * | `cast<T>(expr)`   | opCast, opImplCast, or an inheritance relation |
     *
     * The pass is deliberately silent unless it can see everything it is judging. A target type
     * that resolves to no declaration is engine-registered as far as this analyzer knows, and an
     * engine type can carry conversions that exist nowhere in the source - so it is skipped rather
     * than guessed at. Same for a source expression whose type does not resolve. That asymmetry is
     * the point: a missed error costs nothing, a false one costs the user's trust in every other
     * diagnostic on screen.
     */
    struct TypeConversionCheckRequest
    {
        /** @brief Root node of the document's syntax tree. */
        TSNode root;

        /** @brief Document source text the tree was parsed from. */
        std::string_view sourceCode;

        /** @brief Root of the document's lexical scope tree, or nullptr when none was collected. */
        const Scope *scopeRoot = nullptr;
    };

    /**
     * @brief Walks the document and reports conversions with no declaration to back them.
     * @param request Tree, source text and scope tree of the document under analysis.
     * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
     */
    void CheckTypeConversions(const TypeConversionCheckRequest &request, DiagnosticContext &ctx);
}
