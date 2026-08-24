#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <tree_sitter/api.h>
#include "analysis/DiagnosticContext.h"
#include "analysis/SymbolTable.h"

namespace angel_lsp::analysis
{
    namespace node_types
    {
        constexpr std::string_view StringLiteral = "string_literal";
        constexpr std::string_view LambdaExpression = "lambda_expression";
        constexpr std::string_view BooleanLiteral = "boolean_literal";
        constexpr std::string_view NumberLiteral = "number_literal";
        constexpr std::string_view ImportDeclaration = "import_declaration";
        constexpr std::string_view NullLiteral = "null_literal";
        constexpr std::string_view CallExpression = "call_expression";
    }

    /**
     * @brief Classification categories for initializer list item expressions.
     */
    enum class InitializerItemKind
    {
        NumericOrExpression,
        StringLiteral,
        BooleanLiteral,
        NullLiteral,
        NestedInitializer
    };

    /**
     * @brief Classification categories for enclosing lexical containers.
     */
    enum class ContainerKind
    {
        Class,
        Interface,
        Namespace
    };

    /**
     * @brief Information about an enclosing container in the AST hierarchy.
     */
    struct ContainerInfo
    {
        std::string name;          ///< Bare name, e.g. "Player" or "Game"
        std::string qualifiedName; ///< Fully qualified path, e.g. "Game::Player" or "Game"
        ContainerKind kind;
    };

    /**
     * @brief Checks whether the given name is a reserved AngelScript keyword.
     * @param name Symbol name to check.
     * @return True if name is a reserved keyword.
     */
    bool IsReservedKeyword(const std::string &name);

    /**
     * @brief Checks whether the given name is a primitive AngelScript type name.
     * @param name Symbol or type name to check.
     * @return True if name is a primitive type name.
     */
    bool IsPrimitiveTypeName(const std::string &name);

    /**
     * @brief Semantically classifies the token text of an initializer item.
     * @param item Token or expression text of the initializer item.
     * @return InitializerItemKind classification.
     */
    InitializerItemKind ClassifyInitializerItem(std::string_view item);

    /**
     * @brief True when the symbol was declared in a predefined stub.
     *
     * Stubs describe a host application's API that the engine already accepted, so the rules about
     * how a declaration may be written do not apply to them - a stub's functions have no bodies by
     * design, and judging one as script produces thousands of findings and no information.
     *
     * Shared rather than reimplemented per module: every rule module had its own copy comparing the
     * URI against the configured suffix directly, which meant a stub recognised by the workspace
     * scanner could still be judged as script by the rules. One implementation, delegating to
     * utils::IsPredefinedFile, keeps the two answers from drifting apart again.
     *
     * @param sym Symbol whose declaring file is in question.
     * @param ctx DiagnosticContext carrying the configured stub suffix.
     */
    bool IsFromPredefinedStub(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief True when a function symbol is a destructor declaration.
     *
     * SymbolCollector records `~Foo()` under the bare name "Foo", so a class with both a
     * constructor and a destructor puts two zero-parameter functions in one bucket. Nothing on the
     * signature distinguishes them, so the tilde is read back from the source - which means this
     * only answers for the document under analysis, where sourceCode is available.
     *
     * @param sym Function symbol to classify.
     * @param ctx DiagnosticContext carrying the request's source text and file URI.
     * @return True if the declaration is spelled with a leading '~'.
     */
    bool IsDestructorDeclaration(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief Names the function attribute a declaration carries, or empty when it carries none.
     *
     * The attributes are five independent flags on SymbolModifiers, but every rule that rejects
     * one wants to say which - "cannot carry the 'explicit' attribute" rather than "carries an
     * attribute". Shared because three rule modules ask the same question of three different
     * declaration kinds, and const is deliberately not among them: it is a qualifier the grammar
     * spells separately and several of those declarations accept it.
     *
     * @param modifiers Modifier set to inspect.
     * @return Attribute name in the order the grammar lists them, or an empty view.
     */
    std::string_view FirstAttributeName(const SymbolModifiers &modifiers);

    /**
     * @brief Checks if a given base type name resolves to a mixin class symbol.
     * @param baseTypeName The name of the type to check.
     * @param table SymbolTable to look up the type.
     * @return True if baseTypeName resolves to a mixin class.
     */
    bool IsMixinClass(std::string_view baseTypeName, const class SymbolTable &table);

    /**
     * @brief Checks whether the given base type name is a known type (primitive, string, array, or in SymbolTable).
     * @param baseName Type base name to check.
     * @param ctx DiagnosticContext containing request and SymbolTable.
     * @return True if baseName is a known valid type.
     */
    bool IsKnownType(const std::string &baseName, const DiagnosticContext &ctx);

    /**
     * @brief Strips type modifiers (handles '@', references '&', 'const ', array brackets '[]', 'array<T>').
     * @param typeName Raw type name string or view.
     * @return Cleaned base type name.
     */
    std::string CleanBaseType(std::string_view typeName);

    /**
     * @brief Recursively collects the class and interface inheritance hierarchy for a type.
     * @param className The starting type name.
     * @param symbolTable The symbol table to look up class and interface definitions.
     * @return Vector of type names in the hierarchy including className and its transitive bases.
     */
    std::vector<std::string> GetInheritedTypeHierarchy(const std::string &className, const SymbolTable &symbolTable);

    /**
     * @brief Traverses both base classes/interfaces and derived classes in SymbolTable.
     * @param className The starting class name.
     * @param symbolTable The symbol table to look up class definitions.
     * @return Vector of all related class names across the inheritance graph.
     */
    std::vector<std::string> GetAllRelatedClasses(const std::string &className, const SymbolTable &symbolTable);

    /**
     * @brief Extracts all enclosing container scopes (classes, interfaces, namespaces) for a given AST node.
     * @param node The AST node from which to trace enclosing containers.
     * @param sourceCode Document source text.
     * @return Enclosing containers ordered from innermost to outermost.
     */
    std::vector<ContainerInfo> GetEnclosingContainers(TSNode node, std::string_view sourceCode);

    /**
     * @brief Performs container-aware and inheritance-aware symbol lookup for a name within given containers.
     * @param symbolTable Symbol table.
     * @param containers Enclosing container hierarchy from GetEnclosingContainers.
     * @param name Symbol name to resolve.
     * @return Vector of matching Symbol objects.
     */
    std::vector<Symbol> FindSymbolsInScope(
        const SymbolTable &symbolTable,
        const std::vector<ContainerInfo> &containers,
        const std::string &name);

    /**
     * @brief Performs container-aware and inheritance-aware symbol lookup for a name at an AST node.
     * @param name Symbol name to resolve.
     * @param node The AST node providing lexical context.
     * @param sourceCode Document source text.
     * @param symbolTable Symbol table.
     * @return Vector of matching Symbol objects.
     */
    std::vector<Symbol> FindSymbolsInScope(
        const std::string &name,
        TSNode node,
        std::string_view sourceCode,
        const SymbolTable &symbolTable);

    /**
     * @brief Resolves the resulting type name of an arbitrary AST expression node (identifiers, member chains, calls, etc.).
     * @param exprNode The expression AST node.
     * @param scope Lexical scope if available (can be nullptr).
     * @param symbolTable Symbol table for lookups.
     * @param sourceCode Document source text.
     * @param uri Document file URI.
     * @return Resolved base type name or empty string if unresolved.
     */
    std::string ResolveExpressionType(
        TSNode exprNode,
        const Scope *scope,
        const SymbolTable &symbolTable,
        std::string_view sourceCode,
        std::string_view uri = "");
}


