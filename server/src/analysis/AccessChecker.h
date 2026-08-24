#pragma once

#include "analysis/DiagnosticContext.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Checks that every `object.member` written in a document is allowed to reach it.
     *
     * The first pass in this analyzer that judges how a symbol is *used* rather than how it is
     * declared. Everything else asks whether a declaration is well formed; this asks whether the
     * code at hand may look at one - which needs the expression's type, and so needs the tree.
     *
     * AngelScript's two access modifiers behave exactly as C++'s do, which is not what the wording
     * "private/protected" alone would tell you and is why each rule below was compiled against a
     * real engine before it was written:
     *
     * - `private` is per class, not per instance. Inside class A, `other.q` on a second A is legal;
     *   from anywhere else it is not.
     * - `protected` is reachable from a derived class, but only through an object of that derived
     *   class's own type. In `class C : B : A`, `C c; c.p` is legal and `B bb; bb.p` is not, even
     *   though C inherits p through B.
     *
     * The pass stays silent unless it can see everything the judgement rests on: the object's type,
     * that type's whole base chain, and a declaration of the member. An unresolved link means the
     * member could be anything - an engine-registered type carries members that appear in no
     * source this analyzer reads - and a wrong "illegal access" costs more than a missed one.
     */
    struct AccessCheckRequest
    {
        /** @brief Root node of the document's syntax tree. */
        TSNode root;

        /** @brief Document source text the tree was parsed from. */
        std::string_view sourceCode;

        /** @brief Root of the document's lexical scope tree, or nullptr when none was collected. */
        const Scope *scopeRoot = nullptr;
    };

    /**
     * @brief Walks the document and reports member accesses the declaring class does not permit.
     * @param request Tree, source text and scope tree of the document under analysis.
     * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
     */
    void CheckMemberAccess(const AccessCheckRequest &request, DiagnosticContext &ctx);
}
