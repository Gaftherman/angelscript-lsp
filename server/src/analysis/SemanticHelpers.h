#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <tree_sitter/api.h>
#include "analysis/DiagnosticContext.h"
#include "analysis/SymbolTable.h"
#include "parser/Primitives.h"

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
     * @brief Extracts the raw source text corresponding to an AST node.
     * @param node The AST node.
     * @param sourceCode The document source code.
     * @return Extracted string.
     */
    std::string GetNodeText(TSNode node, std::string_view sourceCode);

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
     * @brief Information about a parsed generic template type (e.g. array<dictionary<string, int>>).
     */
    struct TemplateTypeInfo
    {
        std::string containerName;              ///< Base container name (e.g. "array", "dictionary")
        std::vector<std::string> templateArgs;  ///< Extracted template arguments
    };

    /**
     * @brief Parses a potentially nested template type string into its container name and arguments.
     * @param typeName Raw or specialized type string.
     * @return TemplateTypeInfo with container and nested argument strings.
     */
    TemplateTypeInfo ParseTemplateType(std::string_view typeName);

    /**
     * @brief Normalizes a type name by stripping const, references, and whitespace.
     * @param typeName The raw type name string.
     * @return Cleaned type name.
     */
    std::string CleanExpressionType(std::string_view typeName);

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
     * @brief Immutable core AngelScript primitive types.
     */
    [[nodiscard]] constexpr bool IsCorePrimitive(std::string_view typeName) noexcept
    {
        // `auto` is here and is not a primitive - it is not a type at all, but a stand-in for
        // whatever the initializer produces. Callers that care about the difference test for it by
        // name; see the handle-on-primitive rule in SemanticAnalyzer, which had to.
        return parser::primitives::IsPrimitive(typeName) || typeName == "auto";
    }

    /**
     * @brief True for AngelScript's integer primitives, signed or unsigned.
     *
     * `int32`/`uint32` are the explicit spellings of `int`/`uint` and are listed alongside them:
     * the parser hands back whichever the source wrote, so a classifier that knew only one of each
     * pair would answer differently for two spellings of the same type.
     */
    [[nodiscard]] constexpr bool IsIntegerPrimitive(std::string_view typeName) noexcept
    {
        return parser::primitives::IsInteger(typeName);
    }

    /** @brief True for AngelScript's floating point primitives. */
    /**
     * @brief Strips a namespace qualification, leaving the last segment: `NS::Foo` -> `Foo`.
     *
     * Eight files carried a private copy of these two lines, byte for byte, and a ninth open-coded
     * the same rfind/substr inline. Nothing was wrong with any of them; it is just that a name is
     * either qualified or it is not, and that is one question.
     */
    /**
     * @brief Splits a template argument list at the commas that are not inside a nested `<...>`.
     *
     * `int, array<int, float>, string` gives three, not four: the comma inside the inner list
     * belongs to it. Each piece comes back trimmed, and empty pieces are KEPT - `int,,string` gives
     * three, the middle one empty - because the two callers disagree about what to do with one and
     * that disagreement is theirs to keep. ParseTemplateType drops them; BindTemplateArguments
     * counts them, which is how it notices an argument list that does not match its parameters.
     *
     * The depth counter was written twice, here and in TypeConversionChecker, and this is the part
     * they really shared.
     */
    [[nodiscard]] std::vector<std::string> SplitTemplateArguments(std::string_view inner);

    [[nodiscard]] std::string LastScopeSegment(const std::string &name);

    [[nodiscard]] constexpr bool IsFloatingPointPrimitive(std::string_view typeName) noexcept
    {
        return parser::primitives::IsFloatingPoint(typeName);
    }

    /**
     * @brief True for any primitive that carries a number - integer or floating point.
     *
     * Excludes `bool`, which converts to and from the numeric types but is not one of them; callers
     * that want that conversion handle it explicitly, because whether it is allowed depends on the
     * direction and on which rule is asking.
     *
     * These three predicates exist so the conversion rules, the overload resolver and the
     * expression resolver classify a type the same way. Each had grown its own list, and the lists
     * had already begun to disagree - one of them was missing `int32`.
     */
    [[nodiscard]] constexpr bool IsNumericPrimitive(std::string_view typeName) noexcept
    {
        return IsIntegerPrimitive(typeName) || IsFloatingPointPrimitive(typeName);
    }

    /**
     * @brief Configurable type resolution against host TypeConfig.
     */
    [[nodiscard]] inline bool IsPredefinedOrRegisteredType(
        std::string_view typeName,
        const config::TypeConfig &typeConfig) noexcept
    {
        if (IsCorePrimitive(typeName))
        {
            return true;
        }
        if (typeName == typeConfig.stringTypeName || typeName == typeConfig.arrayTypeName)
        {
            return true;
        }
        return typeConfig.registeredSymbols.contains(std::string(typeName));
    }

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
     * @brief True when a name resolves to a function and to nothing that could be a type.
     *
     * The shape behind `void Foo(int) {}` followed by `Foo@ h`: the compiler answers "Identifier
     * 'Foo' is not a data type" because a function handle needs a funcdef to name its signature.
     *
     * Both halves are required. A name that resolves to nothing at all is an unresolved type and is
     * assumed engine-registered - that is this analyzer's central policy - and a name that resolves
     * to BOTH a function and a type is a legal overload of the two, so neither is worth reporting.
     */
    bool NamesAFunctionNotAType(std::string_view name, const class SymbolTable &table);

    /** @brief What a type is when it cannot be instantiated, for the message that says so. */
    enum class NonInstantiableKind
    {
        None,       ///< The type can be instantiated, or this analyzer cannot see its declaration.
        Abstract,   ///< An abstract class.
        Interface   ///< An interface.
    };

    /**
     * @brief Classifies a type name as an abstract class, an interface, or neither.
     *
     * AngelScript refuses to make an instance of either. A handle to one is the whole point of
     * both and is always legal, so the caller decides that part; this only answers what the type
     * is. Verified against a real engine, which distinguishes them by name in its own messages:
     * "Abstract class 'Shape' cannot be instantiated" against "Interface 'IThing' cannot be
     * instantiated".
     *
     * A name with no visible declaration answers None, like every other rule here: an
     * engine-registered type is exactly what an unresolved name looks like.
     */
    NonInstantiableKind ClassifyNonInstantiable(std::string_view baseTypeName,
                                                const class SymbolTable &table);

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
     * @brief Rewrites the bracket spelling of an array as its template spelling.
     *
     * `int[]` -> `array<int>`, `int[][]` -> `array<array<int>>`; anything without brackets comes
     * back unchanged. The two are the same type to the compiler, and `T[]` is the spelling the
     * language settles on its own - but only one of them survives to member resolution.
     * `CleanBaseType` reduces *both* to `int`, which is correct for its own job (the element type)
     * and useless for finding the container's members, so every site that asks "what type owns
     * this member" has to canonicalise first. Left as-is, `int[] a; a.length();` had no hover, no
     * completion and no call checking, while the identical `array<int> a;` had all three.
     *
     * @param typeName The declared type, in either spelling.
     * @param arrayTypeName The workspace's array container, from `TypeConfig::arrayTypeName`.
     *        Defaulted so a caller with no configuration in reach behaves as it did before, which
     *        is what every hardcoded "array" in this file was already assuming.
     */
    std::string CanonicalizeArrayType(std::string_view typeName, std::string_view arrayTypeName = "array");

    /**
     * @brief The type whose members a `.` on a value of this type reaches.
     *
     * `int[]` and `array<int>` both answer `array`, `Foo@` answers `Foo`, `int` answers `int`.
     *
     * Distinct from `CleanBaseType`, which answers the *element* type and reduces every array -
     * both spellings - to `int`. That is right for the question it is asked most often and wrong
     * for this one, and the two were being conflated: `a.length()` looked for `int::length`, found
     * nothing, and produced no hover for either spelling. The declaration `array<int> a;` happened
     * to work anyway through a hardcoded shortcut for `length`/`size`/`isEmpty`, which is why this
     * read as a bracket-only defect until the template spelling was tried on a fourth method.
     *
     * @param typeName The declared type, with any modifiers still attached.
     * @param arrayTypeName The workspace's array container, from `TypeConfig::arrayTypeName`.
     */
    std::string MemberOwnerType(std::string_view typeName, std::string_view arrayTypeName = "array");

    /**
     * @brief True when a type is AngelScript's variable type `?` (as in `const ?&in`, `?&out`).
     *
     * `?` is not a type name - it is the engine's wildcard parameter, and a parameter declared with
     * it accepts a value of *any* type. dictionary::set(const string&in, const ?&in),
     * dictionary::get(const string&in, ?&out), ref, Dispose and the format/scan helpers are all
     * declared this way in the standard add-ons.
     *
     * Treating `?` as an ordinary named type is why the conversion rules used to report
     * "Cannot implicitly convert 'int' to '?'" on code the real compiler accepts without complaint:
     * the analyzer went looking for a declaration named `?`, found none, and blamed the argument.
     * Anything that compares an argument against a parameter type has to ask this first.
     */
    bool IsVariableType(std::string_view typeName);

    /**
     * @brief Recursively collects the class and interface inheritance hierarchy for a type.
     * @param className The starting type name.
     * @param symbolTable The symbol table to look up class and interface definitions.
     * @return Vector of type names in the hierarchy including className and its transitive bases.
     */
    std::vector<std::string> GetInheritedTypeHierarchy(const std::string &className, const SymbolTable &symbolTable);

    /**
     * @brief The `get_X`/`set_X` methods, anywhere in a type's hierarchy, that stand for `X`.
     *
     * `class E { int get_Health() const property; void set_Health(int) property; }` gives `e.Health`
     * its meaning: measured, the compiler accepts both the read and the write. Nothing about that is
     * visible in the symbol table, which holds two methods and no member called `Health`, so every
     * feature that answers a question about `e.Health` has to derive it - and each one that forgot
     * left the user with a member the compiler accepts and the editor denies.
     *
     * @param keywordRequired asEP_PROPERTY_ACCESSOR_MODE 3, where a method is only a property once
     *        `property` is written. Under mode 2 the name alone is enough. Ask
     *        SemanticAnalysisRequest::RequiresAccessorKeyword, or the engine config, rather than
     *        deciding locally - a setting honoured in some places and not others is worse than one
     *        honoured nowhere.
     * @return The accessors found, getter first when both exist; empty when `X` is not a property.
     */
    std::vector<Symbol> FindPropertyAccessors(const std::string &typeName,
                                              const std::string &propertyName,
                                              const SymbolTable &symbolTable,
                                              bool keywordRequired);

    /**
     * @brief The type a property backed by these accessors carries.
     *
     * The getter's return type, or the setter's parameter when there is no getter - a write-only
     * property is legal and its type is the one it takes.
     */
    std::string PropertyTypeFromAccessors(const std::vector<Symbol> &accessors);

    /**
     * @brief The property name behind an accessor method, or empty when it is an ordinary method.
     *
     * @param keywordRequired As above.
     */
    std::string PropertyNameFromAccessor(const Symbol &sym, bool keywordRequired);

    /**
     * @brief True when every ancestor of a type, and every base each of them names, is in the table.
     *
     * The precondition for any rule that reasons about what a hierarchy contains. A base this
     * server cannot see is a base whose members it cannot enumerate, and "I did not find it" is
     * indistinguishable from "it is not there" - which is how a missing-implementation rule invents
     * an error about a method the invisible base declares. Every such rule stays silent here
     * instead, which is the analyzer's standing answer to a partial view.
     *
     * @param typeName The type whose hierarchy is being judged.
     * @param symbolTable The table to resolve each ancestor and its bases in.
     */
    bool HierarchyIsFullyVisible(const std::string &typeName, const SymbolTable &symbolTable);

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
     * @brief Whether `node` is the callee of a legal `super(...)` base-constructor call.
     *
     * `super` is not a general reference to the base class. The compiler answers
     * "No matching symbol 'super'" for both `super.F()` and `super::F()` - the idiom for a base
     * method is `Base::F()`. The one place the word is legal is a constructor calling its base
     * constructor:
     *
     *     class Derived : Base { Derived() { super(1); } }
     *
     * so this returns true only inside a constructor of a class that names a base. Everywhere else
     * `super` really is an undefined identifier and the diagnostic on it is correct, which is why
     * this is a positive test for one shape rather than a blanket skip of the name.
     *
     * @param node Any node whose text is the identifier `super` - the callee of a call expression.
     * @param sourceCode Document source text.
     * @return True when the enclosing function is a constructor of a class with a base list.
     */
    bool IsBaseConstructorCall(TSNode node, std::string_view sourceCode);

    /**
     * @brief Collects all using namespace directives in an AST tree.
     * @param root The root AST node or enclosing container.
     * @param sourceCode Document source text.
     * @return Vector of imported namespace names.
     */
    std::vector<std::string> CollectUsingNamespaces(TSNode root, std::string_view sourceCode);

    /**
     * @brief Checks if a scope prefix is a known namespace, class, interface, enum, or typedef.
     * @param prefix Scope prefix string (e.g. "Geometry", "Parent", "UnknownSpace").
     * @param node AST node for lexical context.
     * @param sourceCode Document source text.
     * @param table Symbol table.
     * @return True if prefix is a valid known scope.
     */
    bool IsKnownScope(const std::string &prefix, TSNode node, std::string_view sourceCode, const SymbolTable &table);

    /**
     * @brief Resolves an unqualified or qualified name within a container chain and active using namespaces.
     * @param symbolTable Symbol table.
     * @param containers Enclosing container hierarchy from GetEnclosingContainers.
     * @param name Symbol name to resolve.
     * @param usingNamespaces List of active using namespace directives visible in this scope.
     * @return Vector of matching Symbol objects.
     */
    std::vector<Symbol> FindSymbolsInScope(
        const SymbolTable &symbolTable,
        const std::vector<ContainerInfo> &containers,
        const std::string &name,
        const std::vector<std::string> &usingNamespaces = {});

    /**
     * @brief Substitutes a generic type parameter (e.g. "T") with a concrete type argument (e.g. "int" or "array<int>").
     * @param typeStr The type string to process.
     * @param paramName The template parameter name to replace (e.g. "T").
     * @param concreteType The replacement type argument.
     * @return Transformed type string with exact identifier boundaries.
     */
    std::string SubstituteTypeParam(std::string_view typeStr, std::string_view paramName, std::string_view concreteType);

    /**
     * @brief Performs container-aware, using-aware and inheritance-aware symbol lookup for a name at an AST node.
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
     * @brief Resolves the resulting type name of an AST expression node.
     *
     * Handles the forms that name a type rather than compute one: `this`, an identifier qualified
     * or not, a member chain, a call, an index, a cast, a constructor call, a unary or postfix
     * operator, and parentheses. Each of those either writes its type down or carries its
     * operand's through, so none of them needs the engine's own rules to answer.
     *
     * Everything else - a literal, a binary or ternary expression, a lambda, an initializer list -
     * returns empty on purpose rather than by omission. Their types come from promotion and
     * operator-overload resolution, which this analyzer does not do, and every caller treats an
     * empty answer as "stay silent". A guess here would turn that silence into a wrong sentence,
     * which costs more than the check that was skipped.
     *
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
        std::string_view uri = "",
        int depth = 0);

    // --- A lambda against the funcdef it is being handed to ---------------------------------
    //
    // Shared because a lambda reaches a funcdef three ways and the compiler judges all three by
    // the same rule: an assignment or initializer (`CB@ cb = function(...)`), a conversion
    // (`CB(function(...))`), and a call argument (`Take(function(...))`). The first two are
    // TypeConversionChecker's, the third is CallChecker's.

    /** @brief One lambda parameter, exactly as the source wrote it. */
    struct LambdaParameter
    {
        bool hasWrittenType = false;  ///< False means "inherit from the funcdef", so unjudgeable.
        std::string typeName;
        bool isConst = false;
        bool isHandle = false;
        bool isReference = false;
        ParameterModifier modifier = ParameterModifier::None;
    };

    /**
     * @brief Reads a `lambda_parameter_list`, which does not wrap its entries.
     *
     * The grammar puts `param_type` and `name` directly on the list node, once per parameter, and
     * leaves `&` and `in`/`out`/`inout` as anonymous tokens between them - so the entries have to
     * be grouped here, by the commas. `const`, the namespace qualifier and the `@` and `[]`
     * suffixes are all inside the `param_type` node's own text.
     *
     * @see BuiltQueries.h, which needs its own query pattern for the same reason.
     */
    std::vector<LambdaParameter> ReadLambdaParameters(TSNode listNode, std::string_view sourceCode);

    /**
     * @brief True when a written lambda signature contradicts the funcdef it is handed to.
     *
     * The compiler compares the WRITTEN signature and does not convert it, so this is an exact
     * match rather than a conversion test. Measured against angelscript_oracle:
     *
     *     funcdef void CB(const string &in);
     *     function(s) { }                  // accepted, the type comes from CB
     *     function(const string &in s) { } // accepted, written and identical
     *     function(string s) { }           // REJECTED - no const, no &in
     *     function(string &in s) { }       // REJECTED - no const
     *     funcdef void CB(int);
     *     function(uint a) { }             // REJECTED - int does not widen here
     *
     * Arity is a hard equality even when every parameter is untyped, and a funcdef's default
     * argument does not relax it: `funcdef void CB(int a = 1)` still rejects `function() { }`.
     *
     * The type NAME is compared by its last `::` segment, and not at all when either side names a
     * typedef - the alias no spelling comparison can see through. That is not caution for its own
     * sake: `typedef float real` against `float`, `array<int>@` against `int[]@`, and a namespaced
     * type written bare inside its namespace are all ACCEPTED by the compiler, and a plain string
     * comparison reports every one. The decorations, by contrast, are compared whatever the name
     * is, because none of them can be hidden by a spelling.
     */
    bool LambdaContradictsFuncdef(const std::vector<LambdaParameter> &lambdaParameters,
                                  const FuncdefSignature &funcdefSig,
                                  const SymbolTable &table);

    /** @brief The `lambda_expression` overload: reads the parameter list off the node itself. */
    bool LambdaContradictsFuncdef(TSNode lambdaNode,
                                  const FuncdefSignature &funcdefSig,
                                  const SymbolTable &table,
                                  std::string_view sourceCode);

    /** @brief True when the node is a lambda in either of the two spellings the grammar uses. */
    [[nodiscard]] bool IsLambdaExpression(TSNode node) noexcept;

    /**
     * @brief The funcdef a type name denotes, or nullopt.
     *
     * Falls back to a last-`::`-segment scan when the qualified name resolves to nothing, the way
     * a bare name inside a namespace has to.
     */
    std::optional<Symbol> FindFuncdefSymbol(const std::string &typeName, const SymbolTable &table);

    /**
     * @brief The funcdef a lambda is being handed to, read from where the lambda is written.
     *
     * A lambda has no return type of its own - the grammar gives `lambda_expression` a parameter
     * list and a body and nothing else - so every question about what its body must return has to
     * come from the target. Handles the three shapes a lambda reaches a funcdef through: a
     * declaration whose written type is a funcdef, a funcdef used as a conversion, and an argument
     * landing on a funcdef parameter. Anything else answers nullopt.
     */
    std::optional<Symbol> FuncdefTargetOfLambda(TSNode lambdaNode,
                                                const SymbolTable &table,
                                                std::string_view sourceCode);
}


