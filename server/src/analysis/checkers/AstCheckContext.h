#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/mediator/SemanticMediator.h"

#include <concepts>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Immutable AST and lexical inputs shared across all semantic AST checkers.
 */
struct AstCheckContext
{
    std::string_view sourceCode{};
    const Scope* scopeRoot = nullptr;
    const NodeIndex* nodeIndex = nullptr;
    const SemanticMediator* mediator = nullptr;
};

/**
 * @brief Static contract for stateless AST semantic checkers executed by SemanticAnalyzer.
 */
template <typename T>
concept SemanticAstChecker = requires(const AstCheckContext& ast, DiagnosticContext& ctx) {
    { T::Name } -> std::convertible_to<std::string_view>;
    { T::IsEnabled(ctx.request) } -> std::convertible_to<bool>;
    { T::Run(ast, ctx) } -> std::same_as<void>;
};

/**
 * @brief Conditionally executes a semantic AST checker if enabled in the analysis request.
 * @tparam Checker Type satisfying the SemanticAstChecker concept.
 * @param[in] ast AST check context containing the syntax tree and source view.
 * @param[in,out] ctx Diagnostic context accumulating emitted findings.
 */
template <SemanticAstChecker Checker> inline void RunChecker(const AstCheckContext& ast, DiagnosticContext& ctx)
{
    if (Checker::IsEnabled(ctx.request))
    {
        Checker::Run(ast, ctx);
    }
}

/**
 * @brief Executes a pack of semantic AST checkers in order against the given context.
 * @tparam Checkers Pack of types satisfying the SemanticAstChecker concept.
 * @param[in] ast AST check context containing the syntax tree and source view.
 * @param[in,out] ctx Diagnostic context accumulating emitted findings.
 */
template <SemanticAstChecker... Checkers> inline void RunCheckers(const AstCheckContext& ast, DiagnosticContext& ctx)
{
    (RunChecker<Checkers>(ast, ctx), ...);
}

} // namespace angel_lsp::analysis
