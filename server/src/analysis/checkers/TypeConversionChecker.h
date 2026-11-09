#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/types/TypeConversionValues.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks that every conversion written in a document has a declaration backing it.
 *        AngelScript reaches a target type through four routes, and each one is a declaration this
 */

struct TypeConversionCheckRequest
{
    /**
     * @brief Extracted syntax from the same revision as the source and scope tree.
     */
    const NodeIndex& nodeIndex;

    /**
     * @brief Document source text the tree was parsed from.
     */
    std::string_view sourceCode;

    /**
     * @brief Root of the document's lexical scope tree, or nullptr when none was collected.
     */
    const Scope* scopeRoot = nullptr;

    /**
     * @brief The same tree as scopeRoot, non-null only while the caller exclusively owns it.
     *        `auto` inference writes the deduced type back into the LocalDefinition so that everything
     */
    Scope* mutableScopeRoot = nullptr;

    /// Binary expression and if-ladder facts; CheckTypeConversions builds them once when left null.
    const BinaryExpressionFacts* binaryFacts = nullptr;
};

/**
 * @brief Evaluates indexed conversion sites and reports unsupported conversions.
 * @param request Tree, source text and scope tree of the document under analysis.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void CheckTypeConversions(const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Whether a value of `fromType` can reach `toType` implicitly.
 *        The same judgement CheckTypeConversions makes internally, exposed for the one caller that has
 * @param fromType Source type, as spelled - decorations are stripped internally.
 * @param toType Target type, likewise.
 * @param ctx Diagnostic sink; used only for the SymbolTable and config it carries.
 */
bool CanConvertImplicitly(std::string_view fromType, std::string_view toType, const DiagnosticContext& ctx);

/**
 * @brief Evaluates whether a type satisfies boolean truthiness in control flow conditions.
 *        In AngelScript, conditions (if, while, for, do-while, ternary) accept:
 * @param typeName Type name to evaluate.
 * @param table SymbolTable for looking up class declarations and methods.
 * @return True if the type can evaluate as a condition; false otherwise.
 */
bool IsTruthyCondition(std::string_view typeName, const SymbolTable& table);

/**
 * @brief Left and right operand types and value categories in an assignment expression.
 */
struct AssignmentOperands
{
    TSNode left;
    TSNode right;
};

/**
 * @brief Analysis engine for recursive expression value evaluation and type convertibility queries.
 */
struct TypeConversionEvaluator
{
    /**
     * @brief Determines whether a value of source type is convertible to target type.
     * @param[in] from Source type name string.
     * @param[in] to Target destination type name string.
     * @param[in] ctx Diagnostic context containing symbol table and active type configuration.
     * @param[in,out] state Active conversion exploration state and edge guards.
     * @return True if convertible; false otherwise.
     */
    static bool IsConvertible(std::string_view from, std::string_view to, const DiagnosticContext& ctx,
                              ConversionState& state);

    /**
     * @brief Resolves the expression type and constness denotation for an AST value node.
     * @param[in] node AST expression node to resolve.
     * @param[in] scope Lexical scope.
     * @param[in] ctx Diagnostic sink and evaluation context.
     * @return Resolved ExpressionType.
     */
    static ExpressionType ResolveValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx);
};
} // namespace angel_lsp::analysis
