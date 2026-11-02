#pragma once

#include "analysis/index/SymbolTable.h"
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
bool AreIncompatibleTemplateTypes(std::string_view from, std::string_view to);
std::string SubstituteTypeParam(std::string_view typeStr, std::string_view paramName, std::string_view concreteType);
TemplateBinding BindTemplateArguments(const std::string& writtenType, const SymbolTable& table);
std::string SubstituteTemplateParameters(std::string_view typeStr, const TemplateBinding& binding);
std::string QualifyTypeInContainer(std::string_view typeStr, std::string_view containerScope, const SymbolTable& table);
std::string ResolveIndexedType(std::string_view typeName, size_t indexCount, const SymbolTable& symbolTable,
                               std::string_view arrayTypeName);
std::string ResolveIndexedTypeWithArg(std::string_view typeName, const SymbolTable& symbolTable,
                                      std::string_view arrayTypeName, std::string_view indexArgType);
} // namespace angel_lsp::analysis
