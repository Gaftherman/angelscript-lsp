#pragma once

#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Checks that every initializer list is written against a type that accepts one.
     *
     * `{ ... }` is not a general-purpose initializer in AngelScript. It is only valid where the
     * target type registered a list factory, and the primitives never do - so `int x = {5};` and,
     * less obviously, the stray brace in `array<int> a = {1, 2, {3}, 4};` are both errors. The
     * compiler answers each with `Initialization lists cannot be used with 'int'`, because it
     * matches the list against the element pattern and the element there is a plain `int`.
     *
     * This pass was written from that compiler's answers rather than from the grammar: it existed
     * as a known parity gap - a script the real compiler rejected and this analyzer accepted in
     * silence - until the nesting rule was traced case by case against AS-Harness.
     */
    struct InitializerListCheckRequest
    {
        /** @brief Root node of the document's syntax tree. */
        TSNode root;

        /** @brief Document source text the tree was parsed from. */
        std::string_view sourceCode;

        /**
         * @brief Root of the document's lexical scope tree, or nullptr when none was collected.
         *
         * Needed to type an element that names something - `array<int> a = {count}` - rather than
         * spelling a literal. Without it the element check still runs on literals and stays silent
         * on everything else, which is the correct degradation.
         */
        const Scope *scopeRoot = nullptr;
    };

    /**
     * @brief Reports initializer lists applied to a type that cannot be built from one.
     * @param request Tree and source text of the document under analysis.
     * @param ctx Diagnostic sink; also carries the TypeConfig naming the array template.
     */
    void CheckInitializerLists(const InitializerListCheckRequest &request, DiagnosticContext &ctx);

    /**
     * @brief Checks one list against a target type the caller already knows.
     *
     * A list written as a call argument is compiled against the parameter it lands on, and which
     * parameter that is depends on the overload the compiler picks - a question this pass cannot
     * answer and CallChecker already has. So the argument case is driven from there: it names the
     * type, this names the verdict, and the rules stay in one place.
     *
     * @param listNode   The `initializer_list` node.
     * @param targetType The type it initializes, as written; decorations are stripped here.
     * @param sourceCode Document text the tree was parsed from.
     * @param scope      Scope the list sits in, for typing elements that name something.
     * @param ctx        Diagnostic sink.
     */
    void CheckInitializerListAgainstType(TSNode listNode,
                                         const std::string &targetType,
                                         std::string_view sourceCode,
                                         const Scope *scope,
                                         DiagnosticContext &ctx);
}
