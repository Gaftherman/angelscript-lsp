#include "analysis/resolution/CallCandidates.h"

#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/Utils.h"

#include <memory>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Strips qualifiers and template decorations from a receiver type name.
 * @param[in] rawType Raw type text.
 * @return Normalized base type name view.
 */
static std::string_view CleanReceiverTypeName(std::string_view rawType) noexcept
{
    TrimTypeWhitespace(rawType);
    rawType = StripLeadingConst(rawType);
    TrimTypeWhitespace(rawType);
    rawType = StripTrailingDecorationsView(rawType, false);
    TrimTypeWhitespace(rawType);

    if (rawType.ends_with("[]") || rawType.starts_with("array<"))
    {
        return "array";
    }
    if (const auto pos = rawType.find('<'); pos != std::string_view::npos)
    {
        return rawType.substr(0, pos);
    }
    return rawType;
}

/**
 * @brief Resolves the declared type name of a receiver identifier.
 * @param[in] receiverName Name of receiver identifier or "this".
 * @param[in] scope Active lexical scope; it owns the local definitions the result may view.
 * @param[in] symbolTable Symbol table.
 * @param[out] symbolsHandle Receives the symbol snapshot that owns the returned text when it comes from the table.
 * @return View into the resolved type name, valid while @p scope and @p symbolsHandle live; empty if unresolved.
 */
static std::string_view ResolveReceiverType(std::string_view receiverName, const Scope* scope,
                                            const SymbolTable& symbolTable,
                                            std::shared_ptr<const std::vector<Symbol>>& symbolsHandle)
{
    if (receiverName == "this")
    {
        return scope ? scope->GetEnclosingClass() : std::string_view{};
    }
    if (const auto* def = ResolveInScope(scope, receiverName))
    {
        return def->typeName;
    }
    if (scope && !scope->GetEnclosingClass().empty())
    {
        symbolsHandle = symbolTable.FindMemberSymbolPtr(scope->GetEnclosingClass(), receiverName);
        if (symbolsHandle && !symbolsHandle->empty() && (*symbolsHandle)[0].type == SymbolType::Variable)
        {
            return (*symbolsHandle)[0].GetVariable().typeName;
        }
    }
    symbolsHandle = symbolTable.FindSymbolsPtr(receiverName);
    if (symbolsHandle)
    {
        for (const auto& sym : *symbolsHandle)
        {
            if (sym.type == SymbolType::Variable)
            {
                return sym.GetVariable().typeName;
            }
        }
    }
    return {};
}

/**
 * @brief Resolves candidate method symbols for a member expression callee.
 * @param[in] receiver Receiver expression name.
 * @param[in] callee Callee method identifier.
 * @param[in] scope Current lexical scope.
 * @param[in] symbolTable Symbol table.
 * @return Matching symbol candidates from the receiver hierarchy.
 */
static std::vector<Symbol> ResolveMemberCandidates(std::string_view receiver, std::string_view callee,
                                                   const Scope* scope, const SymbolTable& symbolTable)
{
    std::shared_ptr<const std::vector<Symbol>> receiverSymbols;
    const std::string_view rawType = ResolveReceiverType(receiver, scope, symbolTable, receiverSymbols);
    if (rawType.empty())
    {
        return {};
    }
    const std::string_view cleanType = CleanReceiverTypeName(rawType);
    const auto hierarchy = GetInheritedTypeHierarchy(cleanType, symbolTable);
    std::vector<Symbol> candidates;

    for (const auto& typeName : hierarchy)
    {
        const auto mems = symbolTable.FindMemberSymbols(typeName, callee);
        for (const auto& sym : mems)
        {
            if (sym.type == SymbolType::Function || sym.type == SymbolType::Funcdef)
            {
                candidates.push_back(sym);
            }
        }
        if (!candidates.empty())
        {
            return candidates;
        }
    }
    return {};
}

/**
 * @brief Resolves signature information for direct (non-member) callee expressions.
 * @param[in] callee Callee identifier.
 * @param[in] prevToken Token preceding callee if declaring variable.
 * @param[in] scope Current lexical scope.
 * @param[in] symbolTable Symbol table.
 * @return Resolved list of candidate symbols.
 */
static std::vector<Symbol> ResolveDirectCandidates(std::string_view callee, std::string_view prevToken,
                                                   const Scope* scope, const SymbolTable& symbolTable)
{
    const LocalDefinition* localDef = ResolveInScope(scope, callee);
    if (localDef)
    {
        if (auto fd = FindFuncdefSymbol(CleanBaseTypeView(localDef->typeName), symbolTable))
        {
            return {FuncdefToFunctionSymbol(*fd, callee)};
        }
    }

    const std::string_view initType =
        (localDef && !localDef->typeName.empty()) ? std::string_view(localDef->typeName) : prevToken;
    if (!initType.empty())
    {
        const std::string_view cleanInit = CleanBaseTypeView(initType);
        auto ctors = symbolTable.FindMemberSymbols(cleanInit, cleanInit);
        if (!ctors.empty())
        {
            return ctors;
        }
    }

    if (const auto found = symbolTable.FindSymbolsPtr(callee); found && !found->empty())
    {
        std::vector<Symbol> funcs;
        for (const auto& s : *found)
        {
            if (s.type == SymbolType::Function || s.type == SymbolType::Funcdef)
            {
                funcs.push_back(s);
            }
        }
        if (!funcs.empty())
        {
            return funcs;
        }
    }

    auto ctors = symbolTable.FindMemberSymbols(callee, callee);
    if (!ctors.empty())
    {
        return ctors;
    }
    const auto typeSyms = symbolTable.FindTypeSymbolsByShortName(callee);
    if (!typeSyms.empty())
    {
        auto typeCtors = symbolTable.FindMemberSymbols(typeSyms[0].name, typeSyms[0].name);
        if (!typeCtors.empty())
        {
            return typeCtors;
        }
    }

    if (scope && !scope->GetEnclosingClass().empty())
    {
        const auto mems = symbolTable.FindMemberSymbols(scope->GetEnclosingClass(), callee);
        std::vector<Symbol> funcs;
        for (const auto& s : mems)
        {
            if (s.type == SymbolType::Function || s.type == SymbolType::Funcdef)
            {
                funcs.push_back(s);
            }
        }
        if (!funcs.empty())
        {
            return funcs;
        }
    }

    if (auto callableSym = TryResolveCallableFuncdef(callee, scope, symbolTable))
    {
        return {*callableSym};
    }
    return {};
}

std::vector<Symbol> ResolveCallCandidates(const IndexedCallContext& context, std::string_view source,
                                          const Scope* scope, const SymbolTable& symbolTable)
{
    const std::string_view callee = utils::SliceSource(source, context.calleeStartByte, context.calleeEndByte);
    if (callee.empty())
    {
        return {};
    }

    if (context.kind == CallContextKind::Member)
    {
        const std::string_view receiver =
            utils::SliceSource(source, context.receiverStartByte, context.receiverEndByte);
        return ResolveMemberCandidates(receiver, callee, scope, symbolTable);
    }

    if (context.kind == CallContextKind::Qualified)
    {
        const std::string_view qualifier =
            utils::SliceSource(source, context.receiverStartByte, context.receiverEndByte);
        const auto found =
            qualifier.empty() ? symbolTable.FindSymbolsPtr(callee) : symbolTable.FindMemberSymbolPtr(qualifier, callee);
        return found ? *found : std::vector<Symbol>{};
    }

    const std::string_view prevToken = utils::SliceSource(source, context.prevTokenStartByte, context.prevTokenEndByte);
    return ResolveDirectCandidates(callee, prevToken, scope, symbolTable);
}

} // namespace angel_lsp::analysis
