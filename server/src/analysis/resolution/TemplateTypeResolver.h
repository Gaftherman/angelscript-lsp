#pragma once

#include "analysis/index/SymbolTable.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
struct TemplateTypeInfo
{
    std::string containerName;
    std::vector<std::string> templateArgs;
};

struct TemplateBinding
{
    bool isTemplate = false;
    bool usable = false;
    std::vector<std::string> parameters;
    std::vector<std::string> arguments;
};

TemplateTypeInfo ParseTemplateType(std::string_view typeName);

/**
 * @brief Returns the declared template parameter count when `typeName` resolves to a template class.
 * @param[in] typeName Unqualified or scoped base type name (without `<...>`).
 * @param[in] table Symbol table to query.
 * @return Number of template parameters, or `std::nullopt` if `typeName` is not a template class.
 */
std::optional<size_t> GetTemplateParameterCount(std::string_view typeName, const SymbolTable& table);
bool AreIncompatibleTemplateTypes(std::string_view from, std::string_view to);
std::string SubstituteTypeParam(std::string_view typeStr, std::string_view paramName, std::string_view concreteType);
TemplateBinding BindTemplateArguments(std::string_view writtenType, const SymbolTable& table);
std::string SubstituteTemplateParameters(std::string_view typeStr, const TemplateBinding& binding);
std::string QualifyTypeInContainer(std::string_view typeStr, std::string_view containerScope, const SymbolTable& table);
std::string ResolveIndexedType(std::string_view typeName, size_t indexCount, const SymbolTable& symbolTable,
                               std::string_view arrayTypeName);
std::string ResolveIndexedTypeWithArg(std::string_view typeName, const SymbolTable& symbolTable,
                                      std::string_view arrayTypeName, std::string_view indexArgType);
} // namespace angel_lsp::analysis
