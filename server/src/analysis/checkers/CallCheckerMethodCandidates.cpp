#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

bool IsConstructorOrDestructorName(const std::string& memberName, const std::string& typeName)
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

    return memberName.front() == '~' && std::string_view(memberName).substr(1) == shortName;
}

std::vector<Symbol> FindMethodCandidates(const std::string& typeName, const std::string& methodName,
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

static void SubstituteFunctionTemplateParams(FunctionSignature& fn, const std::string& paramName,
                                             const std::string& concreteType)
{
    fn.returnType = SubstituteTypeParam(fn.returnType, paramName, concreteType);
    for (auto& p : fn.parameters)
    {
        p.typeName = SubstituteTypeParam(p.typeName, paramName, concreteType);
        p.baseTypeName = SubstituteTypeParam(p.baseTypeName, paramName, concreteType);
    }
}

void ApplyTemplateSubstitutions(std::vector<Symbol>& candidates, const std::string& typeName,
                                const std::vector<std::string>& fallbackArgs, const SymbolTable& table)
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

bool IsEnclosingMethodConst(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode curr = parser::FindAncestorWithSymbol(node, gs.symFuncDeclaration);
    if (!ts_node_is_null(curr))
    {
        TSTreeCursor cursor = ts_tree_cursor_new(curr);
        bool isConstMethod = false;
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode child = ts_tree_cursor_current_node(&cursor);
                if (ts_node_symbol(child) == gs.symFuncAttributes)
                {
                    isConstMethod = !ts_node_is_null(parser::FindChildWithSymbol(child, gs.tokConst));
                    break;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);
        return isConstMethod;
    }
    return false;
}

static std::optional<Symbol> TryResolveGetterCallableFuncdef(const std::string& typeName, const std::string& memberName,
                                                             const SymbolTable& table)
{
    const std::string getterName = "get_" + memberName;
    if (const auto getterFound = table.FindMemberSymbolPtr(typeName, getterName))
    {
        for (const auto& sym : *getterFound)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                if (fn.parameters.empty())
                {
                    std::string clean = CleanBaseType(fn.returnType);
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

std::optional<Symbol> TryResolveMemberCallableFuncdef(const std::string& objectType, const std::string& memberName,
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
                    std::string clean = CleanBaseType(sym.GetVariable().typeName);
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
