#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks what a function body does with control flow.
 *
 * The only pass here that reads statements rather than declarations. It needs the tree because
 * none of what it judges reaches the symbol table: whether a `break` sits inside a loop, whether
 * two `case` labels name the same value, whether every path out of a non-void function returns.
 *
 * These questions are answerable from one document alone - unlike almost everything else in
 * this analyzer, no host-registered type can change the answer - so the pass says what it finds
 * rather than falling silent on unresolved names.
 */
struct ControlFlowCheckRequest
{
    /** @brief Root node of the document's syntax tree. */
    TSNode root;

    /** @brief Document source text the tree was parsed from. */
    std::string_view sourceCode;
};

/**
 * @brief Walks the document's statements and reports control-flow errors.
 * @param request Tree and source text of the document under analysis.
 * @param ctx Diagnostic sink.
 */
void CheckControlFlow(const ControlFlowCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Tracks branch termination, return paths, and unreachable code states across AST blocks.
 */
struct ControlFlowState
{
    uint32_t loopDepth = 0;
    uint32_t switchDepth = 0;

    /**
     * @brief What the enclosing function must return, and the name to say it under.
     *
     * Empty means there is nothing to check against - a constructor, or a lambda whose
     * funcdef this analyzer cannot see - and the rule stays silent, which is the policy
     * everywhere else the world is only partly visible.
     */
    std::string requiredReturn;
    std::string functionName;

    /**
     * @brief True for a function the grammar gives no return type: a constructor or a
     *        destructor.
     *
     * Both return void, and `return 42;` in one is "Can't return value when return type is
     * 'void'" - measured. as-err-void-return-value covers the same mistake in a function
     * that spells `void` out, and it reads the return type node, so it never fired here.
     */
    bool implicitVoid = false;
};

/**
 * @brief Queue item representing an active control flow node and its accumulated exit states.
 */
struct FlowItem
{
    TSNode node;
    ControlFlowState state;
};
} // namespace angel_lsp::analysis
