#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/AccessorNames.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

bool IsConstructorOrDestructorName(std::string_view memberName, std::string_view typeName)
{
    if (memberName.empty() || typeName.empty())
    {
        return false;
    }

    const std::string_view shortName = LastScopeSegment(typeName);

    if (memberName == shortName)
    {
        return true;
    }

    return memberName.front() == '~' && memberName.substr(1) == shortName;
}

std::vector<Symbol> FindMethodCandidates(std::string_view typeName, std::string_view methodName,
                                         const SymbolTable& table, ExpressionTypeCache* cache)
{
    std::vector<Symbol> candidates;
    for (const auto& owner : GetInheritedTypeHierarchy(typeName, table, cache))
    {
        const auto found = table.FindMemberSymbolPtr(owner, methodName);
        if (!found)
        {
            continue;
        }
        for (const auto& sym : *found)
        {
            if (!IsFunctionSymbol(sym))
            {
                continue;
            }
            if (sym.containerKind == ContainerKind::Namespace || sym.containerKind == ContainerKind::Enum)
            {
                continue;
            }

            const bool overriddenLower =
                std::any_of(candidates.begin(), candidates.end(),
                            [&sym](const Symbol& kept)
                            {
                                return HasSameParameterList(kept, sym) &&
                                       kept.GetFunction().modifiers.isConst == sym.GetFunction().modifiers.isConst;
                            });
            if (!overriddenLower)
            {
                candidates.push_back(sym);
            }
        }
    }
    return candidates;
}

static void SubstituteFunctionTemplateParams(FunctionSignature& fn, std::string_view paramName,
                                             std::string_view concreteType)
{
    fn.returnType = SubstituteTypeParam(fn.returnType, paramName, concreteType);
    for (auto& p : fn.parameters)
    {
        p.typeName = SubstituteTypeParam(p.typeName, paramName, concreteType);
        p.baseTypeName = SubstituteTypeParam(p.baseTypeName, paramName, concreteType);
    }
}

void ApplyTemplateSubstitutions(std::vector<Symbol>& candidates, std::string_view typeName,
                                std::span<const std::string> fallbackArgs, const SymbolTable& table)
{
    const auto binding = BindTemplateArguments(typeName, table);
    if (binding.usable)
    {
        for (auto& sym : candidates)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                auto fn = sym.GetFunction();
                for (size_t i = 0; i < binding.parameters.size(); ++i)
                {
                    SubstituteFunctionTemplateParams(fn, binding.parameters[i], binding.arguments[i]);
                }
                sym.signature = fn;
            }
        }
    }
    else if (!fallbackArgs.empty())
    {
        for (auto& sym : candidates)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                auto fn = sym.GetFunction();
                for (auto& p : fn.parameters)
                {
                    p.typeName = SubstituteTypeParam(p.typeName, "T", fallbackArgs[0]);
                    p.baseTypeName = SubstituteTypeParam(p.baseTypeName, "T", fallbackArgs[0]);
                }
                sym.signature = fn;
            }
        }
    }
}

void FilterMethodCandidatesConstness(std::vector<Symbol>& candidates, bool isReceiverConst)
{
    if (isReceiverConst)
    {
        const bool hasConst = std::any_of(candidates.begin(), candidates.end(),
                                          [](const Symbol& sym)
                                          {
                                              return sym.type == SymbolType::Function &&
                                                     std::holds_alternative<FunctionSignature>(sym.signature) &&
                                                     sym.GetFunction().modifiers.isConst;
                                          });
        if (hasConst)
        {
            std::erase_if(candidates,
                          [](const Symbol& sym)
                          {
                              return sym.type == SymbolType::Function &&
                                     std::holds_alternative<FunctionSignature>(sym.signature) &&
                                     !sym.GetFunction().modifiers.isConst;
                          });
        }
        else
        {
            candidates.clear();
        }
    }
    else
    {
        std::erase_if(candidates,
                      [&candidates](const Symbol& sym)
                      {
                          if (sym.type != SymbolType::Function ||
                              !std::holds_alternative<FunctionSignature>(sym.signature) ||
                              !sym.GetFunction().modifiers.isConst)
                          {
                              return false;
                          }
                          return std::any_of(candidates.begin(), candidates.end(),
                                             [&sym](const Symbol& other)
                                             {
                                                 return &sym != &other && other.type == SymbolType::Function &&
                                                        std::holds_alternative<FunctionSignature>(other.signature) &&
                                                        !other.GetFunction().modifiers.isConst &&
                                                        HasSameParameterList(sym, other);
                                             });
                      });
    }
}

static std::optional<Symbol> TryResolveGetterCallableFuncdef(std::string_view typeName, std::string_view memberName,
                                                             const SymbolTable& table)
{
    char getterBuf[512];
    const std::string_view getterName = utils::ComposeGetterName(memberName, getterBuf);
    if (const auto getterFound = getterName.empty() ? nullptr : table.FindMemberSymbolPtr(typeName, getterName))
    {
        for (const auto& sym : *getterFound)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                if (fn.parameters.empty())
                {
                    const std::string_view clean = CleanBaseTypeView(fn.returnType);
                    if (!clean.empty())
                    {
                        if (auto funcdefSym = FindFuncdefSymbol(clean, table))
                        {
                            return FuncdefToFunctionSymbol(*funcdefSym, memberName);
                        }
                    }
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<Symbol> TryResolveMemberCallableFuncdef(std::string_view objectType, std::string_view memberName,
                                                      const SymbolTable& table, ExpressionTypeCache* cache)
{
    const auto hierarchy = GetInheritedTypeHierarchy(objectType, table, cache);
    for (const auto& typeName : hierarchy)
    {
        if (const auto found = table.FindMemberSymbolPtr(typeName, memberName))
        {
            for (const auto& sym : *found)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                    std::holds_alternative<VariableSignature>(sym.signature))
                {
                    const std::string_view clean = CleanBaseTypeView(sym.GetVariable().typeName);
                    if (!clean.empty())
                    {
                        if (auto funcdefSym = FindFuncdefSymbol(clean, table))
                        {
                            return FuncdefToFunctionSymbol(*funcdefSym, memberName);
                        }
                    }
                }
            }
        }
        if (auto getterFunc = TryResolveGetterCallableFuncdef(typeName, memberName, table))
        {
            return getterFunc;
        }
    }
    return std::nullopt;
}

} // namespace angel_lsp::analysis
