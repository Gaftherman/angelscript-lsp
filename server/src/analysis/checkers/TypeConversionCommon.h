#pragma once

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/checkers/TypeConversionChecker.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/GrammarNames.h"

#include <ankerl/unordered_dense.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

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
 * @param[in] str Input string.
 * @return Trimmed string copy.
 */
std::string TrimString(std::string str);

/**
 * @brief Collects candidate symbols of type Function matching a name.
 * @param[in] funcName Target function name or qualified name.
 * @param[in] table Workspace symbol table.
 * @return Vector of function symbols matching the name.
 */
std::vector<Symbol> CollectFunctionCandidates(const std::string& funcName, const SymbolTable& table);

/**
 * @brief Finds a funcdef symbol matching the given name in the symbol table.
 * @param[in] name Funcdef type name to search.
 * @param[in] table Symbol table.
 * @return Optional Symbol if found.
 */
std::optional<Symbol> FindFuncdef(const std::string& name, const SymbolTable& table);

/**
 * @brief Looks up what a type name denotes.
 * @param[in] typeName Name of the type to inspect.
 * @param[in] table Symbol table to query.
 * @return TypeDeclarationInfo indicating kind and template status.
 */
TypeDeclarationInfo FindTypeDeclaration(const std::string& typeName, const SymbolTable& table);

/**
 * @brief True when a name resolves to an enum declared somewhere the analyzer can see.
 * @param[in] typeName Name of the candidate enum type.
 * @param[in] table Symbol table to query.
 * @return True if name resolves to an enum; false otherwise.
 */
bool ResolvesToEnum(const std::string& typeName, const SymbolTable& table);

/**
 * @brief True when a name resolves to an enum, typedef, funcdef or namespace.
 * @param[in] typeName Name to inspect.
 * @param[in] table Symbol table to query.
 * @return True if name resolves to a non-class declaration; false otherwise.
 */
bool ResolvesToNonClassDeclaration(const std::string& typeName, const SymbolTable& table);

/**
 * @brief Visits every overload of a named method visible on a type, base types included.
 * @param[in] typeName Target type name.
 * @param[in] memberName Method name to look up.
 * @param[in] table Symbol table to query.
 * @param[in] visitor Callback returning true to stop traversal.
 */
void ForEachMethod(const std::string& typeName, const std::string& memberName, const SymbolTable& table,
                   const std::function<bool(const Symbol&)>& visitor);

/**
 * @brief Visits the constructors declared directly on a type.
 * @param[in] typeName Target type name.
 * @param[in] table Symbol table to query.
 * @param[in] visitor Callback returning true to stop traversal.
 */
void ForEachConstructor(const std::string& typeName, const SymbolTable& table,
                        const std::function<bool(const Symbol&)>& visitor);

/**
 * @brief True when an overload can be called with exactly one argument.
 * @param[in] parameters Function parameters vector.
 * @return True if function accepts a single argument; false otherwise.
 */
bool AcceptsSingleArgument(const std::vector<ParameterInformation>& parameters);

/**
 * @brief The declared type of the single argument such an overload converts from.
 * @param[in] parameters Function parameters vector.
 * @return Canonicalized type name of the first argument.
 */
std::string SingleArgumentType(const std::vector<ParameterInformation>& parameters);

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
bool DeclaresOperatorMethod(const std::string& typeName, std::string_view methodName, const SymbolTable& table);

/**
 * @brief True for a type no cast<> may name: a primitive, or an enum.
 * @param[in] typeName Type name to inspect.
 * @param[in] table Symbol table to query.
 * @return True if type is a scalar cast target; false otherwise.
 */
bool IsScalarCastTarget(const std::string& typeName, const SymbolTable& table);

/**
 * @brief True when a type declares any cast operator overload.
 * @param[in] typeName Type name to inspect.
 * @param[in] table Symbol table to query.
 * @return True if opCast or opImplCast is declared; false otherwise.
 */
bool DeclaresAnyCastOperator(const std::string& typeName, const SymbolTable& table);

/**
 * @brief True when a type declares a conversion operator producing the target type.
 * @param[in] fromType Source type name.
 * @param[in] toType Destination type name.
 * @param[in] table Symbol table to query.
 * @param[in] implicitOnly When true, checks only opImplConv and opImplCast.
 * @return True if conversion operator exists; false otherwise.
 */
bool DeclaresConversionTo(const std::string& fromType, const std::string& toType, const SymbolTable& table,
                          bool implicitOnly);

/**
 * @brief True when derived is derived from base (valid implicit upcast).
 * @param[in] derived Derived type name.
 * @param[in] base Base type name.
 * @param[in] table Symbol table to query.
 * @return True if derived inherits base; false otherwise.
 */
bool IsDerivedFrom(const std::string& derived, const std::string& base, const SymbolTable& table);

/**
 * @brief True when one type appears in the other's inheritance chain, either direction.
 * @param[in] a First type name.
 * @param[in] b Second type name.
 * @param[in] table Symbol table to query.
 * @return True if types are related in hierarchy; false otherwise.
 */
bool AreHierarchyRelated(const std::string& a, const std::string& b, const SymbolTable& table);

/**
 * @brief The workspace's string type, which defaults to string when unconfigured.
 * @param[in] typeName Type name to check.
 * @param[in] ctx Diagnostic context carrying config.
 * @return True if type represents the string type; false otherwise.
 */
bool IsStringType(const std::string& typeName, const DiagnosticContext& ctx);

/**
 * @brief Checks whether a type is a known primitive or string value type.
 * @param[in] typeName Type name to check.
 * @param[in] ctx Diagnostic context carrying config.
 * @return True if builtin value type; false otherwise.
 */
bool IsBuiltInValueType(const std::string& typeName, const DiagnosticContext& ctx);

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
bool IsConvertible(const std::string& from, const std::string& to, const DiagnosticContext& ctx, size_t depth = 0);

/**
 * @brief Resolves the ExpressionType resulting from an explicit cast AST node.
 * @param[in] node AST expression node to resolve.
 * @param[in] scope Lexical scope at node position.
 * @param[in] ctx Diagnostic context.
 * @return Resolved ExpressionType.
 */
ExpressionType ResolveValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx);

/**
 * @brief Inspects property access metadata on a syntax node.
 * @param[in] exprNode Expression node accessing property.
 * @param[in] scope Enclosing lexical scope.
 * @param[in] ctx Diagnostic context.
 * @return PropertyAccessInfo structure.
 */
PropertyAccessInfo InspectPropertyAccess(TSNode exprNode, const Scope* scope, const DiagnosticContext& ctx);

/**
 * @brief Validates property access mutations on assignment LHS.
 * @param[in] node Assignment syntax node.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckAssignmentPropertyAccess(TSNode node, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Emits an ExpressionIsDataType diagnostic unless suppressed due to syntax errors.
 * @param[in] node AST expression node.
 * @param[in] ctx Diagnostic context.
 * @param[in] dataTypeName Name of the data type used as an expression.
 */
void EmitExpressionIsDataType(TSNode node, const DiagnosticContext& ctx, std::string_view dataTypeName);

/**
 * @brief Validates signedness compatibility in binary comparison expressions.
 * @param[in] fact Binary comparison facts.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckSignedUnsignedComparison(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Emits a warning when a float value implicitly converts to an integer.
 * @param[in] valueNode Expression node being converted.
 * @param[in] types Source and destination types.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckFloatTruncation(TSNode valueNode, const ConversionTypePair& types, const Scope* scope,
                          DiagnosticContext& ctx);

/**
 * @brief Reads a 'type' node into the shape the conversion rules can act on.
 * @param[in] typeNode AST type node.
 * @param[in] ctx Diagnostic context.
 * @return DeclaredType structure.
 */
DeclaredType ReadDeclaredType(TSNode typeNode, const DiagnosticContext& ctx);

/**
 * @brief Validates non-handle declarator construction semantics.
 * @param[in] child Variable declarator syntax node.
 * @param[in] declared Declared type details.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckDeclaratorConstruction(TSNode child, const DeclaredType& declared, const Scope* scope,
                                 DiagnosticContext& ctx);

/**
 * @brief Rule for a one-argument construction: T(expr) or T v(expr);.
 * @param[in] argumentListNode Argument list AST node.
 * @param[in] targetType Target destination type name.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckConstruction(TSNode argumentListNode, const std::string& targetType, const Scope* scope,
                       DiagnosticContext& ctx);

/**
 * @brief Checks that a non-handle variable declaration provides a valid default constructor.
 * @param[in] declaratorNode Declarator syntax node.
 * @param[in] typeName Target type name.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckDefaultConstructor(TSNode declaratorNode, const std::string& typeName, DiagnosticContext& ctx);

/**
 * @brief Checks for disallowed constructor delegation statements inside constructor bodies.
 * @param[in] funcNode Function declaration AST node.
 * @param[in] index Syntax index.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckConstructorDelegation(TSNode funcNode, const NodeIndex& index, DiagnosticContext& ctx);

/**
 * @brief Validates funcdef assignment compatibility against a target funcdef signature.
 * @param[in] targetNode Diagnostic anchor node.
 * @param[in] funcdefSig Expected funcdef signature.
 * @param[in] valueNode Assigned value node.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckFuncdefAssignment(TSNode targetNode, const FuncdefSignature& funcdefSig, TSNode valueNode,
                            DiagnosticContext& ctx);

/**
 * @brief Resolves the lexical scope enclosing a syntax tree node.
 * @param[in] node AST node to resolve scope for.
 * @param[in] request Analysis request carrying scope root.
 * @return Enclosing scope pointer, or nullptr if none found.
 */
const Scope* ResolveNodeScope(TSNode node, const TypeConversionCheckRequest& request);

/**
 * @brief Finds the boolean conversion operator (opImplConv or opConv) for a class.
 * @param[in] typeName Target type name.
 * @param[in] table Symbol table to query.
 * @return Pointer to operator name string, or nullptr if none.
 */
const std::string* BoolConversionOperator(const std::string& typeName, const SymbolTable& table);

/**
 * @brief Validates condition semantics for control flow statements.
 * @param[in] node Control flow syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessConditionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates ternary expression branches for type compatibility.
 * @param[in] node Ternary expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessTernaryNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates expression statements that bare data type names.
 * @param[in] node Expression statement syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessExpressionStatementNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates container iteration in foreach statements.
 * @param[in] node Foreach statement node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessForeachNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates variable declaration nodes across auto and explicit types.
 * @param[in] node Variable declaration syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessVariableDeclarationNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates auto-inferred variable declarations.
 * @param[in] node Variable declaration syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessAutoVariableDeclarators(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates assignment expression semantics and type compatibility.
 * @param[in] node Assignment expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessAssignmentNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates binary expression operators and operand type compatibility.
 * @param[in] node Binary expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessBinaryNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates unary and postfix increment/decrement operations.
 * @param[in] node Unary or postfix expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessUnaryOrPostfixNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates member expressions against write-only properties.
 * @param[in] node Member expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessMemberExpressionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates index expressions against write-only indexed properties.
 * @param[in] node Index expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessIndexExpressionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates explicit cast expression safety.
 * @param[in] node Cast expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessCastNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates return statements across enclosing function or lambda bodies.
 * @param[in] node Return statement syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessReturnStatementNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates call and explicit construct call expressions.
 * @param[in] node Call expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessCallOrConstructNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
