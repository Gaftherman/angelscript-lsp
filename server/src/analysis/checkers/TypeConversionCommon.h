#pragma once

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/checkers/TypeConversionChecker.h"
#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/DeclarationSiteFacts.h"
#include "analysis/index/ExpressionSiteFacts.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <ankerl/unordered_dense.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Cursor tracking progress through a sequence of indexed type conversion fact sites.
 */
struct TypeConversionSiteCursor
{
    /**
     * @brief Accessor retrieving the start byte of the indexed site at the given index.
     */
    uint32_t (*getByte)(size_t index, const TypeConversionCheckRequest& request) = nullptr;

    /**
     * @brief Dispatch callback evaluating the indexed site at the given index.
     */
    void (*dispatch)(size_t index, const TypeConversionCheckRequest& request, DiagnosticContext& ctx) = nullptr;

    /**
     * @brief Total number of indexed sites in this cursor's sequence.
     */
    size_t size = 0;

    /**
     * @brief Current site index within the sequence.
     */
    size_t index = 0;

    /**
     * @brief Returns the start byte offset of the current site, or UINT32_MAX when exhausted.
     * @param[in] request Type conversion request containing the indexed fact vectors.
     * @return Start byte offset or UINT32_MAX.
     */
    [[nodiscard]] uint32_t currentByte(const TypeConversionCheckRequest& request) const noexcept
    {
        return (index < size) ? getByte(index, request) : UINT32_MAX;
    }
};

/**
 * @brief Visits every symbol registered under a qualified name without copying the bucket.
 * @param[in] qualifiedName Fully qualified symbol name.
 * @param[in] table Symbol table to query.
 * @param[in] visitor Callback returning true to stop traversal.
 */
void ForEachSymbolNamed(std::string_view qualifiedName, const SymbolTable& table,
                        const std::function<bool(const Symbol&)>& visitor);

/**
 * @brief Visits every symbol registered under a member of a scope without copying the bucket.
 * @param[in] scope Enclosing scope name.
 * @param[in] member Member name to look up.
 * @param[in] table Symbol table to query.
 * @param[in] visitor Callback returning true to stop traversal.
 */
void ForEachMemberSymbolNamed(std::string_view scope, std::string_view member, const SymbolTable& table,
                              const std::function<bool(const Symbol&)>& visitor);

/**
 * @brief Trims leading and trailing whitespace from a string.
 * @param[in] str Input string; the result views the same storage.
 * @return View of the input without surrounding whitespace.
 */
std::string_view TrimString(std::string_view str);

/**
 * @brief Collects candidate symbols of type Function matching a name.
 * @param[in] funcName Target function name or qualified name.
 * @param[in] table Workspace symbol table.
 * @return Vector of function symbols matching the name.
 */
std::vector<Symbol> CollectFunctionCandidates(std::string_view funcName, const SymbolTable& table);

/**
 * @brief Finds a funcdef symbol matching the given name in the symbol table.
 * @param[in] name Funcdef type name to search.
 * @param[in] table Symbol table.
 * @return Optional Symbol if found.
 */
std::optional<Symbol> FindFuncdef(std::string_view name, const SymbolTable& table);

/**
 * @brief Looks up what a type name denotes.
 * @param[in] typeName Name of the type to inspect.
 * @param[in] table Symbol table to query.
 * @return TypeDeclarationInfo indicating kind and template status.
 */
TypeDeclarationInfo FindTypeDeclaration(std::string_view typeName, const SymbolTable& table);

/**
 * @brief True when a name resolves to an enum, typedef, funcdef or namespace.
 * @param[in] typeName Name to inspect.
 * @param[in] table Symbol table to query.
 * @return True if name resolves to a non-class declaration; false otherwise.
 */
bool ResolvesToNonClassDeclaration(std::string_view typeName, const SymbolTable& table);

/**
 * @brief Visits every overload of a named method visible on a type, base types included.
 * @param[in] typeName Target type name.
 * @param[in] memberName Method name to look up.
 * @param[in] table Symbol table to query.
 * @param[in] visitor Callback returning true to stop traversal.
 */
void ForEachMethod(std::string_view typeName, std::string_view memberName, const SymbolTable& table,
                   const std::function<bool(const Symbol&)>& visitor);

/**
 * @brief Visits the constructors declared directly on a type.
 * @param[in] typeName Target type name.
 * @param[in] table Symbol table to query.
 * @param[in] visitor Callback returning true to stop traversal.
 */
void ForEachConstructor(std::string_view typeName, const SymbolTable& table,
                        const std::function<bool(const Symbol&)>& visitor);

/**
 * @brief True when an overload can be called with exactly one argument.
 * @param[in] parameters Function parameters vector.
 * @return True if function accepts a single argument; false otherwise.
 */
bool AcceptsSingleArgument(const std::vector<ParameterInformation>& parameters);

/**
 * @brief The overload an assignment operator calls, or empty for one that has none.
 * @param[in] op Assignment operator text.
 * @return Corresponding method name or empty view.
 */
std::string_view AssignmentOverloadName(std::string_view op);

/**
 * @brief Whether typeName declares the operator method methodName at all.
 * @param[in] typeName Target type name.
 * @param[in] methodName Operator method name.
 * @param[in] table Symbol table to query.
 * @return True if operator method is declared; false otherwise.
 */
bool DeclaresOperatorMethod(std::string_view typeName, std::string_view methodName, const SymbolTable& table);

/**
 * @brief True for a type no cast<> may name: a primitive, or an enum.
 * @param[in] typeName Type name to inspect.
 * @param[in] table Symbol table to query.
 * @return True if type is a scalar cast target; false otherwise.
 */
bool IsScalarCastTarget(std::string_view typeName, const SymbolTable& table);

/**
 * @brief True when a type declares any cast operator overload.
 * @param[in] typeName Type name to inspect.
 * @param[in] table Symbol table to query.
 * @return True if opCast or opImplCast is declared; false otherwise.
 */
bool DeclaresAnyCastOperator(std::string_view typeName, const SymbolTable& table);

/**
 * @brief True when a type declares a conversion operator producing the target type.
 * @param[in] fromType Source type name.
 * @param[in] toType Destination type name.
 * @param[in] table Symbol table to query.
 * @param[in] implicitOnly When true, checks only opImplConv and opImplCast.
 * @return True if conversion operator exists; false otherwise.
 */
bool DeclaresConversionTo(std::string_view fromType, std::string_view toType, const SymbolTable& table,
                          bool implicitOnly);

/**
 * @brief True when one type appears in the other's inheritance chain, either direction.
 * @param[in] a First type name.
 * @param[in] b Second type name.
 * @param[in] table Symbol table to query.
 * @return True if types are related in hierarchy; false otherwise.
 */
bool AreHierarchyRelated(std::string_view a, std::string_view b, const SymbolTable& table);

/**
 * @brief The workspace's string type, which defaults to string when unconfigured.
 * @param[in] typeName Type name to check.
 * @param[in] ctx Diagnostic context carrying config.
 * @return True if type represents the string type; false otherwise.
 */
bool IsStringType(std::string_view typeName, const DiagnosticContext& ctx);

/**
 * @brief Checks whether a type is a known primitive or string value type.
 * @param[in] typeName Type name to check.
 * @param[in] ctx Diagnostic context carrying config.
 * @return True if builtin value type; false otherwise.
 */
bool IsBuiltInValueType(std::string_view typeName, const DiagnosticContext& ctx);

/**
 * @brief Checks if either type is empty or an error poison sentinel.
 * @param[in] from Source type name.
 * @param[in] to Destination type name.
 * @return True if either type is poison or empty; false otherwise.
 */
bool IsPoisonOrEmptyType(std::string_view from, std::string_view to);

/**
 * @brief Decides whether a value of type 'from' can reach type 'to'.
 * @param[in] from Source type name.
 * @param[in] to Destination type name.
 * @param[in] ctx Diagnostic context.
 * @param[in] depth Current recursion depth guard.
 * @return True whenever a route exists; false otherwise.
 */
bool IsConvertible(std::string_view from, std::string_view to, const DiagnosticContext& ctx, size_t depth = 0);

/**
 * @brief Resolves the ExpressionType of a value expression captured in pass 1.
 * @param[in] value Leaf kind, spelling and unary operators of the expression.
 * @param[in] scope Lexical scope at the expression position.
 * @param[in] ctx Diagnostic context.
 * @return Resolved ExpressionType; a poison type for expressions with syntax errors, unknown when unresolved.
 */
ExpressionType ResolveValueType(const ValueExpressionFact& value, const Scope* scope, const DiagnosticContext& ctx);

/**
 * @brief Inspects the accessor metadata of an indexed expression that may name a virtual property.
 * @param[in] access Indexed expression spelling and spans.
 * @param[in] scope Enclosing lexical scope.
 * @param[in] ctx Diagnostic context.
 * @return PropertyAccessInfo structure; no flag is set for an expression that names no property.
 */
PropertyAccessInfo InspectPropertyAccess(const PropertyAccessFact& access, const Scope* scope,
                                         const DiagnosticContext& ctx);

/**
 * @brief Validates property access mutations on assignment LHS.
 * @param[in] site Indexed assignment with its left property access.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckAssignmentPropertyAccess(const AssignmentSiteFact& site, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Emits an ExpressionIsDataType diagnostic for an indexed expression unless suppressed due to syntax errors.
 * @param[in] expression Indexed expression fact.
 * @param[in] ctx Diagnostic context.
 * @param[in] dataTypeName Name of the data type used as an expression.
 */
void EmitExpressionIsDataType(const ConversionExpressionFact& expression, const DiagnosticContext& ctx,
                              std::string_view dataTypeName);

/**
 * @brief Validates signedness compatibility in binary comparison expressions.
 * @param[in] fact Binary comparison facts.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckSignedUnsignedComparison(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Emits a warning when an indexed float operand implicitly converts to an integer.
 * @param[in] value Indexed operand being converted.
 * @param[in] types Source and destination types.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckFloatTruncation(const BinaryOperandFact& value, const ConversionTypePair& types, const Scope* scope,
                          DiagnosticContext& ctx);

/**
 * @brief Reads a written type spelling into the shape the conversion rules can act on.
 * @param[in] raw Type spelling as written, including qualifiers and decorations.
 * @param[in] ctx Diagnostic context.
 * @return DeclaredType structure.
 */
DeclaredType ReadDeclaredType(std::string_view raw, const DiagnosticContext& ctx);

/**
 * @brief Validates non-handle declarator construction semantics.
 * @param[in] declarator Variable declarator fact.
 * @param[in] declared Declared type details.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckDeclaratorConstruction(const VariableDeclaratorFact& declarator, const DeclaredType& declared,
                                 const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Rule for a one-argument construction: T(expr) or T v(expr);.
 * @param[in] argument Sole construction argument; an absent argument ends the check.
 * @param[in] targetType Target destination type name.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckConstruction(const ConstructionArgumentFact& argument, std::string_view targetType, const Scope* scope,
                       DiagnosticContext& ctx);

/**
 * @brief Checks that a non-handle variable declaration provides a valid default constructor.
 * @param[in] target Range the diagnostic is anchored to.
 * @param[in] typeName Target type name.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckDefaultConstructor(const SourceRange& target, std::string_view typeName, DiagnosticContext& ctx);

/**
 * @brief Reports every statement of a constructor body that delegates to the class itself.
 * @param[in] site Delegating constructor site; its slice selects the offending statements.
 * @param[in] facts Declaration facts that own the slice.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckConstructorDelegation(const ConstructorDelegationSiteFact& site, const DeclarationSiteFacts& facts,
                                DiagnosticContext& ctx);

/**
 * @brief Validates an indexed funcdef value against a target funcdef signature.
 * @param[in] target Diagnostic anchor range.
 * @param[in] funcdefSig Expected funcdef signature.
 * @param[in] value Indexed assigned value; an absent value ends the check.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckFuncdefAssignment(SourceRange target, const FuncdefSignature& funcdefSig, const FuncdefValueFact& value,
                            DiagnosticContext& ctx);

/**
 * @brief Resolves the lexical scope enclosing an indexed source position.
 * @param[in] at Span whose start selects the scope.
 * @param[in] request Analysis request carrying scope root.
 * @return Enclosing scope pointer, or nullptr if none found.
 */
const Scope* ResolveNodeScope(const SourceRange& at, const TypeConversionCheckRequest& request);

/**
 * @brief Finds the boolean conversion operator (opImplConv or opConv) for a class.
 * @param[in] typeName Target type name.
 * @param[in] table Symbol table to query.
 * @return Name of the boolean conversion operator, or an empty view when the type has none.
 */
std::string_view BoolConversionOperator(std::string_view typeName, const SymbolTable& table);

/**
 * @brief Validates condition semantics for control flow statements and ternary expressions.
 * @param[in] site Indexed condition site.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessConditionSite(const ConditionSiteFact& site, const TypeConversionCheckRequest& request,
                          DiagnosticContext& ctx);

/**
 * @brief Validates ternary expression branches for type compatibility.
 * @param[in] site Indexed ternary site.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessTernarySite(const TernarySiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates expression statements that bare data type names.
 * @param[in] site Indexed expression statement site.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessExpressionStatementSite(const ExpressionStatementSiteFact& site, const TypeConversionCheckRequest& request,
                                    DiagnosticContext& ctx);

/**
 * @brief Validates container iteration in foreach statements and infers loop variable types.
 * @param[in] site Indexed foreach site.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessForeachSite(const ForeachSiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates a variable declaration across auto and explicit types.
 * @param[in] site Variable declaration fact.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessVariableDeclarationSite(const VariableDeclarationSiteFact& site, const TypeConversionCheckRequest& request,
                                    DiagnosticContext& ctx);

/**
 * @brief Validates auto-inferred variable declarations.
 * @param[in] site Indexed `auto` declaration site.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessAutoDeclarationSite(const AutoDeclarationSiteFact& site, const TypeConversionCheckRequest& request,
                                DiagnosticContext& ctx);

/**
 * @brief Validates assignment expression semantics and type compatibility.
 * @param[in] site Indexed assignment with its operands.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessAssignmentSite(const AssignmentSiteFact& site, const TypeConversionCheckRequest& request,
                           DiagnosticContext& ctx);

/**
 * @brief Validates binary expression operators and operand type compatibility for an indexed site.
 * @param[in] site Binary expression site fact.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessBinarySite(const BinarySiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates unary and postfix increment/decrement operations for an indexed site.
 * @param[in] site Unary or postfix expression site fact.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessUnaryOrPostfixSite(const UnaryPostfixSiteFact& site, const TypeConversionCheckRequest& request,
                               DiagnosticContext& ctx);

/**
 * @brief Validates member expressions against write-only properties for an indexed site.
 * @param[in] site Member expression site fact.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessMemberExpressionSite(const MemberExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                 DiagnosticContext& ctx);

/**
 * @brief Validates index expressions against write-only indexed properties for an indexed site.
 * @param[in] site Index expression site fact.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessIndexExpressionSite(const IndexExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                DiagnosticContext& ctx);

/**
 * @brief Validates explicit cast expression safety for an indexed site.
 * @param[in] site Cast expression site fact.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessCastSite(const CastExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                     DiagnosticContext& ctx);

/**
 * @brief Validates a return statement against the declared return type of its function or lambda target.
 * @param[in] site Indexed return statement with its owner data.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessReturnSite(const ReturnSiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates a call or explicit construct call.
 * @param[in] site Call or construct call fact.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessCallConstructSite(const CallConstructSiteFact& site, const TypeConversionCheckRequest& request,
                              DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
