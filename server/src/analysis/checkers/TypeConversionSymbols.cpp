#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

void ForEachSymbolNamed(std::string_view qualifiedName, const SymbolTable& table,
                        const std::function<bool(const Symbol&)>& visitor)
{
    const auto bucket = table.FindSymbolsPtr(qualifiedName);
    if (!bucket)
    {
        return;
    }
    for (const auto& sym : *bucket)
    {
        if (visitor(sym))
        {
            return;
        }
    }
}

void ForEachMemberSymbolNamed(std::string_view scope, std::string_view member, const SymbolTable& table,
                              const std::function<bool(const Symbol&)>& visitor)
{
    const auto bucket = table.FindMemberSymbolPtr(scope, member);
    if (!bucket)
    {
        return;
    }
    for (const auto& sym : *bucket)
    {
        if (visitor(sym))
        {
            return;
        }
    }
}

std::string_view TrimString(std::string_view str)
{
    TrimTypeWhitespace(str);
    return str;
}

std::vector<Symbol> CollectFunctionCandidates(std::string_view funcName, const SymbolTable& table)
{
    std::vector<Symbol> candidates;
    auto addCandidates = [&](std::string_view name)
    {
        if (auto found = table.FindSymbolsPtr(name))
        {
            for (const auto& s : *found)
            {
                if (s.type == SymbolType::Function)
                {
                    candidates.push_back(s);
                }
            }
        }
    };

    addCandidates(funcName);
    if (candidates.empty())
    {
        addCandidates(LastScopeSegment(funcName));
    }
    return candidates;
}

std::optional<Symbol> FindFuncdef(std::string_view name, const SymbolTable& table)
{
    std::optional<Symbol> result;
    ForEachSymbolNamed(name, table,
                       [&](const Symbol& sym)
                       {
                           if (sym.type == SymbolType::Funcdef)
                           {
                               result = sym;
                               return true;
                           }
                           return false;
                       });
    if (result)
    {
        return result;
    }
    const std::string_view bare = LastScopeSegment(name);
    const auto matches = table.FindTypeSymbolsByShortName(bare);
    for (const auto& sym : matches)
    {
        if (sym.type == SymbolType::Funcdef)
        {
            return sym;
        }
    }
    return result;
}

TypeDeclarationInfo FindTypeDeclaration(std::string_view typeName, const SymbolTable& table)
{
    TypeDeclarationInfo info;
    if (typeName.empty())
    {
        return info;
    }

    ForEachSymbolNamed(typeName, table,
                       [&info](const Symbol& sym)
                       {
                           if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
                           {
                               return false;
                           }
                           info.found = true;
                           info.isClass = sym.type == SymbolType::Class;
                           info.isTemplate = info.isClass && sym.GetClass().isTemplate;
                           return true;
                       });

    if (info.found)
    {
        return info;
    }

    const std::string_view bare = LastScopeSegment(typeName);
    const auto matches = table.FindTypeSymbolsByShortName(bare);
    for (const auto& sym : matches)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
        {
            info.found = true;
            info.isClass = sym.type == SymbolType::Class;
            info.isTemplate = info.isClass && sym.GetClass().isTemplate;
            return info;
        }
    }

    return info;
}

bool ResolvesToEnum(const std::string& typeName, const SymbolTable& table)
{
    bool found = false;
    const std::string_view bare = LastScopeSegment(typeName);

    for (const auto candidate : {std::string_view(typeName), bare})
    {
        ForEachSymbolNamed(candidate, table,
                           [&found](const Symbol& sym)
                           {
                               if (sym.type == SymbolType::Enum)
                               {
                                   found = true;
                                   return true;
                               }
                               return false;
                           });

        if (found || bare == typeName)
        {
            break;
        }
    }
    return found;
}

bool ResolvesToNonClassDeclaration(std::string_view typeName, const SymbolTable& table)
{
    bool found = false;
    const std::string_view bare = LastScopeSegment(typeName);

    for (const auto candidate : {std::string_view(typeName), bare})
    {
        ForEachSymbolNamed(candidate, table,
                           [&found](const Symbol& sym)
                           {
                               if (sym.type == SymbolType::Enum || sym.type == SymbolType::Typedef ||
                                   sym.type == SymbolType::Funcdef || sym.type == SymbolType::Namespace)
                               {
                                   found = true;
                                   return true;
                               }
                               return false;
                           });

        if (found || bare == typeName)
        {
            break;
        }
    }
    return found;
}

static bool ClassDeclaresMethodReturningBool(std::string_view cls, std::string_view method, const SymbolTable& table)
{
    if (auto ptr = table.FindMemberSymbolPtr(cls, method))
    {
        for (const auto& sym : *ptr)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                if (CleanBaseType(sym.GetFunction().returnType) == "bool")
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Finds which boolean conversion operator a class declares.
 * @param[in] cls Qualified class name whose members are searched.
 * @param[in] table Symbol table to query.
 * @return `opImplConv` when it returns bool, else `opConv` when it does, else an empty view.
 */
static std::string_view ConversionOperatorOfClass(std::string_view cls, const SymbolTable& table)
{
    constexpr std::string_view implicitName = "opImplConv";
    constexpr std::string_view explicitName = "opConv";
    if (ClassDeclaresMethodReturningBool(cls, implicitName, table))
    {
        return implicitName;
    }
    if (ClassDeclaresMethodReturningBool(cls, explicitName, table))
    {
        return explicitName;
    }
    return {};
}

std::string_view BoolConversionOperator(std::string_view typeName, const SymbolTable& table)
{
    if (typeName.empty())
    {
        return {};
    }

    if (auto ptr = table.FindSymbolsPtr(typeName))
    {
        for (const auto& sym : *ptr)
        {
            if (sym.type == SymbolType::Class)
            {
                return ConversionOperatorOfClass(typeName, table);
            }
        }
    }

    for (const auto& sym : table.FindTypeSymbolsByShortName(typeName))
    {
        if (sym.type == SymbolType::Class)
        {
            return ConversionOperatorOfClass(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName, table);
        }
    }
    return {};
}

bool IsTruthyCondition(std::string_view typeName, const SymbolTable& table)
{
    if (typeName.empty())
    {
        return false;
    }

    if (IsHandleTypeView(typeName))
    {
        return true;
    }

    const auto clean = CleanBaseType(typeName);
    if (clean == "bool")
    {
        return true;
    }

    return !BoolConversionOperator(clean, table).empty();
}

} // namespace angel_lsp::analysis
