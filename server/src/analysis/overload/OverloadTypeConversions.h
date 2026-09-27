#pragma once

#include "analysis/SymbolTable.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Strips const, reference (&, &in, &out), and handle (@) decorations from a type string.
 * @param[in] result Type string to strip.
 * @return Raw base type string.
 */
std::string StripTypeDecorations(std::string result);

/**
 * @brief Desugars array square brackets syntax `T[]` into canonical container template syntax.
 * @param[in] result Type string to desugar.
 * @param[in] arrayTypeName Workspace-configured array container type name.
 * @return Desugared type string.
 */
std::string DesugarArrayBrackets(std::string result, std::string_view arrayTypeName = "array");

/**
 * @brief Normalizes a type name for canonical semantic comparison.
 * @param[in] typeName Raw type name to normalize.
 * @param[in] arrayTypeName Workspace-configured array container type name.
 * @return Canonical normalized type name.
 */
std::string NormalizeType(std::string_view typeName, std::string_view arrayTypeName = "array");

/**
 * @brief Checks whether a type name string carries a handle (@) modifier.
 * @param[in] typeName Type name string to inspect.
 * @return True if handle modifier is present.
 */
bool HasHandleModifier(std::string_view typeName);

/**
 * @brief Formal user conversion result representation.
 */
struct UserConversionMatch
{
    bool viable = false;
    bool isExact = false; ///< True if conversion target matches exactly, false if primitive widening required.
};

/**
 * @brief Checks if a type defines a single-argument converting constructor from fromType.
 * @param[in] fromType Source argument type.
 * @param[in] toType Destination type.
 * @param[in] symbolTable Symbol table for constructor lookup.
 * @return True if converting constructor is available.
 */
bool HasConvertingConstructor(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable);

/**
 * @brief Checks if a type defines an implicit conversion method (`opImplConv` or `opImplCast`).
 * @param[in] fromType Source type.
 * @param[in] toType Target conversion type.
 * @param[in] symbolTable Symbol table for method lookup.
 * @return Conversion match information.
 */
UserConversionMatch CheckConversionMethod(const std::string& fromType, const std::string& toType,
                                          const SymbolTable& symbolTable);

/**
 * @brief Checks if a type defines an implicit conversion method (`opImplConv` or `opImplCast`).
 * @param[in] fromType Source type.
 * @param[in] toType Target conversion type.
 * @param[in] symbolTable Symbol table for method lookup.
 * @return True if conversion method is available.
 */
bool HasConversionMethod(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable);

/**
 * @brief Checks if a user-defined conversion exists between two types.
 * @param[in] fromType Source type.
 * @param[in] toType Destination type.
 * @param[in] symbolTable Symbol table for resolution.
 * @return Conversion match information.
 */
UserConversionMatch CheckUserConversion(const std::string& fromType, const std::string& toType,
                                        const SymbolTable& symbolTable);

/**
 * @brief Checks if a user-defined conversion exists between two types.
 * @param[in] fromType Source type.
 * @param[in] toType Destination type.
 * @param[in] symbolTable Symbol table for resolution.
 * @return True if converting constructor or conversion method exists.
 */
bool HasUserConversion(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable);

/**
 * @brief Recursively unwraps typedef aliases to their canonical underlying types.
 * @param[in] typeName Type name to unwrap.
 * @param[in] symbolTable Symbol table containing typedef definitions.
 * @param[in] depth Current recursion depth.
 * @return Canonical unwrapped type name.
 */
std::string UnwrapTypedef(const std::string& typeName, const SymbolTable& symbolTable, int depth = 0);

/**
 * @brief Checks if a type name corresponds to an integer primitive.
 * @param[in] typeName Type name to inspect.
 * @return True if integer type.
 */
bool IsIntegerType(const std::string& typeName);

/**
 * @brief Checks if a type name corresponds to an unsigned integer primitive.
 * @param[in] typeName Type name to inspect.
 * @return True if unsigned integer type.
 */
bool IsUnsignedInteger(const std::string& typeName);

/**
 * @brief Checks if a type name corresponds to a floating point primitive.
 * @param[in] typeName Type name to inspect.
 * @return True if floating point type.
 */
bool IsFloatingPointType(const std::string& typeName);

/**
 * @brief Checks whether a target type matches any in an initializer list of candidate types.
 * @param[in] target Type name to inspect.
 * @param[in] validTypes List of candidate types.
 * @return True if target is present in validTypes.
 */
bool IsInTypeList(std::string_view target, std::initializer_list<std::string_view> validTypes);

/**
 * @brief Checks whether from primitive can widen to to primitive.
 * @param[in] from Source primitive type name.
 * @param[in] to Destination primitive type name.
 * @return True if valid widening conversion.
 */
bool CheckWideningTarget(std::string_view from, std::string_view to);

/**
 * @brief Checks whether a conversion is a primitive widening conversion.
 * @param[in] fromType Source primitive type.
 * @param[in] toType Destination primitive type.
 * @return True if valid widening.
 */
bool IsPrimitiveWidening(const std::string& fromType, const std::string& toType);

/**
 * @brief Checks whether a conversion is a primitive narrowing conversion.
 * @param[in] fromType Source primitive type.
 * @param[in] toType Destination primitive type.
 * @return True if valid narrowing.
 */
bool IsPrimitiveNarrowing(const std::string& fromType, const std::string& toType);

/**
 * @brief Checks whether a type name string carries a const modifier.
 * @param[in] typeName Type name string to inspect.
 * @return True if const modifier is present.
 */
bool HasConstModifier(std::string_view typeName);

/**
 * @brief Checks whether a parameter accepts wildcard types (? or variable arguments).
 * @param[in] param Parameter information to check.
 * @return True if parameter is wildcard.
 */
bool IsWildcardParameter(const ParameterInformation& param);

/**
 * @brief Checks if a parameter is an output or inout reference parameter.
 * @param[in] param Parameter information to check.
 * @return True if output or inout parameter.
 */
bool IsOutParameter(const ParameterInformation& param);

/**
 * @brief Checks if a parameter represents an array or container type.
 * @param[in] param Parameter information to check.
 * @param[in] symbolTable Optional symbol table to validate custom template container declarations.
 * @param[in] arrayTypeName Optional configured array type name.
 * @return True if container parameter.
 */
bool IsContainerParameter(const ParameterInformation& param, const SymbolTable* symbolTable = nullptr,
                          std::string_view arrayTypeName = "array");

/**
 * @brief Checks if two type names denote the same type, ignoring scope qualifier prefixes.
 * @param[in] a First type name.
 * @param[in] b Second type name.
 * @return True if identical type or matching last scope segment.
 */
bool IsSameType(const std::string& a, const std::string& b);

} // namespace angel_lsp::analysis
