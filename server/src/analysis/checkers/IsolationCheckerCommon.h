#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/IsolationChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Trims leading and trailing whitespace from a text slice and returns an owned string.
 * @param[in] text Source text slice to trim.
 * @return Owned std::string without surrounding whitespace.
 */
std::string TrimWhitespace(std::string_view text);

/**
 * @brief Verifies that the 'shared' modifier is only attached to allowed entity declarations.
 * @param[in] request Contextual request bundle containing AST root, source text, and node index.
 * @param[in,out] ctx Diagnostic context used to emit error diagnostics for invalid shared entities.
 */
void CheckSharedEntityEligibility(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief AST visitor traversing statements and expressions to detect thread isolation and ownership violations.
 */
class IsolationVisitor
{
  public:
    /**
     * @brief Contextual AST check request containing source code, scope hierarchy, and pre-indexed symbols.
     */
    const AstCheckContext& request;

    /**
     * @brief Diagnostic context for reporting violations of shared isolation boundaries.
     */
    DiagnosticContext& ctx;

    /**
     * @brief Name of the class currently being traversed, or empty if traversing free functions or global scope.
     */
    std::string currentClassName;

    /**
     * @brief True if the enclosing class is declared with the shared modifier; false otherwise.
     */
    bool isCurrentClassShared = false;

    /**
     * @brief Constructs an isolation visitor bound to the specified check request and diagnostic sink.
     * @param[in] req Contextual request bundle containing AST root, source text, and node index.
     * @param[in,out] dctx Diagnostic context sink for reporting isolation violations.
     */
    IsolationVisitor(const AstCheckContext& req, DiagnosticContext& dctx);

    /**
     * @brief Determines whether a class declaration or named class symbol possesses the shared modifier.
     * @param[in] node AST node of the class declaration or reference.
     * @param[in] className Name of the candidate class to inspect in the symbol table.
     * @return True if the class has the shared modifier; false otherwise.
     */
    bool IsClassShared(TSNode node, const std::string& className) const;

    /**
     * @brief Determines whether a function declaration is marked shared or inherits shared isolation from an enclosing
     * class.
     * @param[in] node AST node of the function declaration.
     * @param[in] inSharedContext True if the function is nested inside an already shared enclosing context.
     * @return True if the function operates under shared isolation; false otherwise.
     */
    bool IsFunctionShared(TSNode node, bool inSharedContext) const;

    /**
     * @brief Validates that a datatype referenced within a shared context resolves to a shared entity or primitive.
     * @param[in] node AST node representing the datatype expression.
     */
    void CheckSharedDatatype(TSNode node);

    /**
     * @brief Verifies whether a function or method callee name is permitted within the active shared context.
     * @param[in] calleeName Function or method identifier to check.
     * @return True if the callee is accessible from shared code; false otherwise.
     */
    bool IsAllowedCallee(std::string_view calleeName) const;

    /**
     * @brief Validates a function call expression within a shared context to ensure the target function is shared.
     * @param[in] node AST node of the call expression.
     */
    void CheckSharedCallExpression(TSNode node);

    /**
     * @brief Checks if an identifier appears in a declaration, type specification, or callee position rather than a
     * variable read.
     * @param[in] node AST identifier node to evaluate.
     * @return True if the identifier occurs within a non-variable structural context; false otherwise.
     */
    bool IsIdentifierInSpecialContext(TSNode node) const;

    /**
     * @brief Determines whether a variable identifier is a local variable, member property, or registered symbol.
     * @param[in] varName Name of the candidate variable identifier.
     * @param[in] node AST identifier node used for lexical scope resolution.
     * @return True if the variable is permitted in shared contexts; false otherwise.
     */
    bool IsAllowedVariable(std::string_view varName, TSNode node) const;

    /**
     * @brief Validates that an identifier read inside a shared context does not access a non-shared global variable.
     * @param[in] node AST identifier node representing the variable access.
     */
    void CheckSharedIdentifier(TSNode node);

    /**
     * @brief Pushes child nodes of an AST node in reverse order onto the traversal worklist.
     * @param[in] node Parent AST node whose children should be traversed.
     * @param[in] className Enclosing class name to propagate to child work items.
     * @param[in] isClassShared Shared status of the enclosing class to propagate.
     * @param[in] inSharedContext Whether the child nodes execute within a shared boundary.
     * @param[in,out] worklist Depth-first stack of traversal work items.
     */
    void PushChildren(TSNode node, const std::string& className, bool isClassShared, bool inSharedContext,
                      std::vector<IsoItem>& worklist);

    /**
     * @brief Traverses the AST starting from the root node to check all shared isolation invariants.
     * @param[in] rootNode Root syntax tree node from which traversal begins.
     */
    void Visit(TSNode rootNode);
};

} // namespace angel_lsp::analysis
