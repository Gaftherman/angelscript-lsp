#pragma once

#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/TemplateTypeResolver.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Builds the property search order for member expressions, prioritizing non-mixins.
 */
[[nodiscard]] std::vector<std::string> OrderMemberPropertySearchHierarchy(const std::string& cleanObj,
                                                                          const SymbolTable& symbolTable);

/**
 * @brief Finds candidate member symbols in a specific type.
 */
[[nodiscard]] std::vector<Symbol> FindMemberSymbolsInType(const std::string& typeName, const std::string& memName,
                                                          const SymbolTable& symbolTable);

/**
 * @brief Extracts the resolved type of an accessor function member.
 */
[[nodiscard]] std::string ExtractAccessorMemberType(const Symbol& sym);

/**
 * @brief Extracts the resolved type of a member symbol.
 */
[[nodiscard]] std::string ExtractMemberSymbolType(const Symbol& sym, const SymbolTable& table);

/**
 * @brief Resolves known container method return types (length, size, isEmpty).
 */
[[nodiscard]] std::string ResolveContainerMethodFallback(std::string_view memName, const TemplateTypeInfo& templateInfo,
                                                         std::string_view objType, std::string_view arrayTypeName);

/**
 * @brief Resolves return type from candidate method overloads, substituting template arguments.
 */
[[nodiscard]] std::string ResolveCandidateMethodReturn(const std::vector<Symbol>& candidates,
                                                       const std::vector<std::string>& argTypes,
                                                       const std::string& objType, const SymbolTable& symbolTable);

/**
 * @brief Appends candidate function if not already overridden by signature.
 */
void AppendCandidateIfUnique(std::vector<Symbol>& candidates, const Symbol& sym);

/**
 * @brief Collects candidate member methods along the type inheritance hierarchy.
 */
[[nodiscard]] std::vector<Symbol>
CollectMemberMethodCandidates(const std::string& ownerType, const std::string& memName, const SymbolTable& symbolTable);

/**
 * @brief Resolves member property type from object type and member name.
 * @param[in] objType Inferred object receiver type.
 * @param[in] fallbackObjName Fallback object node text.
 * @param[in] memName Member property name.
 * @param[in] symbolTable Symbol table to query.
 * @return Resolved member type, or empty string.
 */
[[nodiscard]] std::string ResolveMemberPropertyType(std::string_view objType, std::string_view fallbackObjName,
                                                    std::string_view memName, const SymbolTable& symbolTable);

} // namespace angel_lsp::analysis
