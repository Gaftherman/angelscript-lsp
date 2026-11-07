#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Keywords.h"
#include "parser/Primitives.h"
#include "utils/AccessorNames.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace angel_lsp::analysis
{
std::string FormatFunctionSignature(const Symbol& sym)
{
    return FormatFunctionDeclaration(sym);
}

std::string FormatClassSignature(const Symbol& sym)
{
    return FormatTypeDeclaration(sym);
}

/**
 * @brief Checks if a container symbol is a class or interface type.
 * @param[in] containerName Container type name.
 * @param[in] symbolTable Optional workspace symbol table.
 * @return True if container is class or interface, false otherwise.
 */
static bool IsContainerProperty(std::string_view containerName, const SymbolTable* symbolTable)
{
    if (containerName.empty() || !symbolTable)
    {
        return !containerName.empty();
    }
    auto containerSyms = symbolTable->FindSymbolsPtr(containerName);
    if (!containerSyms)
    {
        return false;
    }
    for (const auto& cs : *containerSyms)
    {
        if (cs.type == SymbolType::Class || cs.type == SymbolType::Interface)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a variable symbol represents a property of an enclosing class or interface.
 * @param[in] sym Target symbol descriptor.
 * @param[in] symbolTable Optional workspace symbol table.
 * @return True if variable is a property, false otherwise.
 */
static bool IsVariableProperty(const Symbol& sym, const SymbolTable* symbolTable)
{
    if (sym.containerKind == ContainerKind::Enum || sym.containerKind == ContainerKind::Namespace)
    {
        return false;
    }
    if (std::holds_alternative<VariableSignature>(sym.signature) && sym.GetVariable().isEnumConstant)
    {
        return false;
    }
    if (sym.containerKind == ContainerKind::Class || sym.containerKind == ContainerKind::Interface)
    {
        return true;
    }
    return IsContainerProperty(sym.containerName, symbolTable);
}

std::string FormatNamespaceDeclaration(const Symbol& sym)
{
    if (!sym.qualifiedName.empty())
    {
        return "namespace " + sym.qualifiedName;
    }
    if (!sym.containerName.empty())
    {
        return "namespace " + utils::JoinScopedName(sym.containerName, sym.name);
    }
    return "namespace " + sym.name;
}

/**
 * @brief Formats variable or property signature, tagging with enum member, property, or global variable prefix.
 * @param[in] sym Target symbol descriptor.
 * @param[in] symbolTable Optional workspace symbol table.
 * @return Formatted variable or property declaration string.
 */
static std::string FormatVariableOrProperty(const Symbol& sym, const SymbolTable* symbolTable)
{
    if (sym.containerKind == ContainerKind::Enum || sym.GetVariable().isEnumConstant)
    {
        return FormatVariableSignature(sym, "(enum member) ");
    }
    const bool isProperty = IsVariableProperty(sym, symbolTable);
    return FormatVariableSignature(sym, isProperty ? "(property) " : "(global variable) ");
}

/**
 * @brief Renders the core declaration text according to symbol kind.
 * @param[in] sym Target symbol descriptor.
 * @param[in] symbolTable Optional workspace symbol table.
 * @return Formatted declaration string.
 */
static std::string FormatSymbolBody(const Symbol& sym, const SymbolTable* symbolTable)
{
    switch (sym.type)
    {
    case SymbolType::Function:
    case SymbolType::Funcdef:
        return FormatFunctionSignature(sym);
    case SymbolType::Class:
    case SymbolType::Interface:
        return FormatClassSignature(sym);
    case SymbolType::Enum:
    {
        const std::string enumName =
            !sym.containerName.empty() ? utils::JoinScopedName(sym.containerName, sym.name) : sym.name;
        return std::string(parser::keywords::Enum) + " " + enumName;
    }
    case SymbolType::Variable:
        return FormatVariableOrProperty(sym, symbolTable);
    case SymbolType::Typedef:
    {
        const std::string tdName =
            !sym.containerName.empty() ? utils::JoinScopedName(sym.containerName, sym.name) : sym.name;
        return "typedef " + sym.GetTypedef().baseType + " " + tdName;
    }
    case SymbolType::Namespace:
        return FormatNamespaceDeclaration(sym);
    case SymbolType::Property:
        if (std::holds_alternative<VariableSignature>(sym.signature))
        {
            return FormatVariableSignature(sym, "(property) ");
        }
        return "(property) " + sym.name;
    default:
        return sym.name;
    }
}

std::string FormatVariableSignature(const Symbol& sym, const char* role)
{
    if (sym.type != SymbolType::Variable && sym.type != SymbolType::Property)
    {
        return std::string(role) + sym.name;
    }
    if (!std::holds_alternative<VariableSignature>(sym.signature))
    {
        return std::string(role) + sym.name;
    }
    if (sym.containerKind == ContainerKind::Enum || sym.GetVariable().isEnumConstant)
    {
        role = "(enum member) ";
        return std::string(role) + FormatVariableDeclaration(sym.GetVariable(), sym.name);
    }
    std::string displayName = sym.name;
    if (sym.containerKind == ContainerKind::Namespace && !sym.containerName.empty())
    {
        displayName = utils::JoinScopedName(sym.containerName, sym.name);
    }
    return std::string(role) + FormatVariableDeclaration(sym.GetVariable(), displayName);
}

std::string FormatDeclarationText(const Symbol& sym, const SymbolTable* symbolTable)
{
    std::string metaPrefix;
    for (const auto& meta : sym.metadata)
    {
        metaPrefix += meta + "\n";
    }
    return metaPrefix + FormatSymbolBody(sym, symbolTable);
}

void RemoveDuplicateSymbols(std::vector<Symbol>& symbols)
{
    std::vector<Symbol> unique;
    unique.reserve(symbols.size());
    for (const auto& sym : symbols)
    {
        const bool duplicate = std::any_of(unique.begin(), unique.end(),
                                           [&](const Symbol& kept)
                                           {
                                               if (kept.name != sym.name || kept.type != sym.type)
                                                   return false;
                                               if (!sym.fileUri.empty() && !kept.fileUri.empty() &&
                                                   sym.fileUri == kept.fileUri && sym.startLine == kept.startLine)
                                                   return true;
                                               return FormatDeclarationText(kept) == FormatDeclarationText(sym);
                                           });
        if (!duplicate)
        {
            unique.push_back(sym);
        }
    }
    symbols = std::move(unique);
}

std::string FormatSynthesizedPropertyHeader(const std::vector<Symbol>& symbols, std::string_view accessorPropertyType,
                                            std::string_view nodeText)
{
    if (accessorPropertyType.empty())
    {
        return "";
    }
    std::string indexParam;
    for (const auto& sym : symbols)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const auto& fn = sym.GetFunction();
            if ((utils::IsGetterName(sym.name) && !fn.parameters.empty()) ||
                (utils::IsSetterName(sym.name) && fn.parameters.size() > 1))
            {
                indexParam = fn.parameters.front().typeName;
                if (!fn.parameters.front().name.empty())
                {
                    indexParam += " " + fn.parameters.front().name;
                }
                break;
            }
        }
    }
    std::ostringstream oss;
    if (!indexParam.empty())
    {
        oss << "(property) " << accessorPropertyType << " " << nodeText << "[" << indexParam << "]\n";
    }
    else
    {
        oss << "(property) " << accessorPropertyType << " " << nodeText << "\n";
    }
    return oss.str();
}

void AppendFileLocationFooter(const std::vector<Symbol>& symbols, const config::ServerConfig* config,
                              std::ostringstream& oss)
{
    if (config && config->features.hoverShowFileLocation && !symbols.empty() && !symbols.front().fileUri.empty())
    {
        oss << "\n\n---\n*(" << symbols.front().fileUri << ")*";
    }
}
} // namespace angel_lsp::analysis
