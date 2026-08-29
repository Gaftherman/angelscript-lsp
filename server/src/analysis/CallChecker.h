#pragma once

#include "analysis/DiagnosticContext.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Checks that every call passes a number of arguments some declaration can accept.
     *
     * The third use-site pass, beside AccessChecker and ConstChecker. AngelScript answers a call it
     * cannot match with "No matching signatures to 'Take(const int)'" - a failed overload lookup,
     * not a refusal of one signature - so this asks the same question the engine does: is there
     * *any* visible declaration of this name that could take this many arguments?
     *
     * Originally only the count, and this comment said so for a long time after it stopped being
     * true: argument *types* are checked too, through OverloadResolver's ScoreArgumentMatch, which
     * is what picks the best overload and what reports `as-err-no-implicit-conversion` when none of
     * them can take the argument. The paragraph below survives because the reasoning in it still
     * governs where the checking stops.
     *
     * Matching argument types would need the engine's conversion and promotion
     * rules applied across a whole overload set, which this analyzer does not do and which
     * TypeConversionChecker says as much about at its own boundary. The count is decidable from the
     * declarations alone, and it is what catches the mistake people actually make.
     *
     * Silent unless everything is visible, which is the contract the other two carry:
     *
     * - a callee that resolves to no declaration is an engine-registered function, and the corpus
     *   is mostly those;
     * - a call through an object whose type does not resolve, or whose base chain does not, could
     *   reach a declaration written down nowhere here;
     * - one candidate taking `...` accepts any count, so the question is answered and closed.
     */
    struct CallCheckRequest
    {
        /** @brief Root node of the document's syntax tree. */
        TSNode root;

        /** @brief Document source text the tree was parsed from. */
        std::string_view sourceCode;

        /** @brief Root of the document's lexical scope tree, or nullptr when none was collected. */
        const Scope *scopeRoot = nullptr;
    };

    /**
     * @brief Runs the call-argument pass over one document.
     * @param request Tree, source text and scope tree for the document.
     * @param ctx Diagnostic sink and the analysis request behind it.
     */
    void CheckCallArguments(const CallCheckRequest &request, DiagnosticContext &ctx);
}
