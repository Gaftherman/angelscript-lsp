#include "analysis/resolution/CallCandidates.h"

#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <string>
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
    // `T@+` is an auto-handle: the `+` only tells the engine how to hold the reference.
    while (rawType.ends_with('+'))
    {
        rawType.remove_suffix(1);
        TrimTypeWhitespace(rawType);
    }
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
 * @brief Qualifies a field's declared type with the namespaces of the class that declares the field.
 * @param[in] declaredType Type text as written next to the field, for example `ASLogger@`.
 * @param[in] ownerClass Qualified name of the declaring class.
 * @param[in] symbolTable Symbol table.
 * @return The type spelled with the enclosing namespace that defines it when the field's class lives in one and the
 * name is not already qualified; otherwise @p declaredType unchanged.
 */
static std::string QualifyTypeFromOwner(std::string_view declaredType, std::string_view ownerClass,
                                        const SymbolTable& symbolTable)
{
    const std::string_view bare = CleanReceiverTypeName(declaredType);
    if (utils::ContainsScopeSeparator(bare))
    {
        return std::string(declaredType);
    }
    for (std::string_view scope = utils::ParentScopeView(ownerClass); !scope.empty();
         scope = utils::ParentScopeView(scope))
    {
        const std::string candidate = utils::JoinScopedName(scope, bare);
        if (symbolTable.FindSymbolsPtr(candidate))
        {
            return candidate;
        }
    }
    return std::string(declaredType);
}

/**
 * @brief Finds the declared type of a field or virtual property reachable from a class through its ancestors.
 * @param[in] typeName Class whose hierarchy is searched, ancestors included.
 * @param[in] memberName Field or property name.
 * @param[in] symbolTable Symbol table.
 * @return Declared type text, or an empty string when no field or `get_`/`set_` accessor has that name.
 */
static std::string MemberTypeThroughHierarchy(std::string_view typeName, std::string_view memberName,
                                              const SymbolTable& symbolTable)
{
    for (const auto& owner : GetInheritedTypeHierarchy(typeName, symbolTable))
    {
        if (const auto found = symbolTable.FindMemberSymbolPtr(owner, memberName);
            found && !found->empty() && (*found)[0].type == SymbolType::Variable)
        {
            return QualifyTypeFromOwner((*found)[0].GetVariable().typeName, owner, symbolTable);
        }
    }
    return PropertyTypeFromAccessors(FindPropertyAccessors(typeName, memberName, symbolTable, false));
}

/**
 * @brief Resolves the declared type name of a receiver expression.
 * @param[in] receiverText Receiver text: `this`, an identifier, or a dotted chain such as `this.owner`.
 * @param[in] scope Active lexical scope; it owns the local definitions the result may view.
 * @param[in] symbolTable Symbol table.
 * @return The receiver's declared type, which may come from a local, a field or accessor of the enclosing class or
 * an ancestor, or a global; empty if unresolved.
 */
static std::string ResolveReceiverType(std::string_view receiverText, const Scope* scope,
                                       const SymbolTable& symbolTable)
{
    if (const auto dot = receiverText.rfind('.'); dot != std::string_view::npos)
    {
        const std::string head = ResolveReceiverType(receiverText.substr(0, dot), scope, symbolTable);
        if (head.empty())
        {
            return {};
        }
        return MemberTypeThroughHierarchy(CleanReceiverTypeName(head), receiverText.substr(dot + 1), symbolTable);
    }
    if (receiverText == "this")
    {
        return scope ? std::string(scope->GetEnclosingClass()) : std::string{};
    }
    if (const auto* def = ResolveInScope(scope, receiverText))
    {
        return def->typeName;
    }
    if (scope && !scope->GetEnclosingClass().empty())
    {
        if (auto inherited = MemberTypeThroughHierarchy(scope->GetEnclosingClass(), receiverText, symbolTable);
            !inherited.empty())
        {
            return inherited;
        }
    }
    if (const auto found = symbolTable.FindSymbolsPtr(receiverText))
    {
        for (const auto& sym : *found)
        {
            if (sym.type == SymbolType::Variable)
            {
                return sym.GetVariable().typeName;
            }
        }
    }
    return PropertyTypeFromAccessors(FindGlobalPropertyAccessors(receiverText, symbolTable));
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
    const std::string rawType = ResolveReceiverType(receiver, scope, symbolTable);
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
            else if (sym.type == SymbolType::Variable)
            {
                // A field of funcdef type is called like a method: `holder.callback(`.
                if (const auto fd = FindFuncdefSymbol(CleanBaseTypeView(sym.GetVariable().typeName), symbolTable))
                {
                    candidates.push_back(FuncdefToFunctionSymbol(*fd, callee));
                }
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
        auto hierarchy = GetInheritedTypeHierarchy(scope->GetEnclosingClass(), symbolTable);
        ExpandMixinHostHierarchies(scope->GetEnclosingClass(), symbolTable, hierarchy);
        for (const auto& owner : hierarchy)
        {
            std::vector<Symbol> funcs;
            for (const auto& s : symbolTable.FindMemberSymbols(owner, callee))
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
    }

    if (auto callableSym = TryResolveCallableFuncdef(callee, scope, symbolTable))
    {
        return {*callableSym};
    }
    return {};
}

/**
 * @brief Resolves the candidates of a `Qualifier::callee(` call.
 * @param[in] qualifier Namespace or class written before `::`.
 * @param[in] callee Name written after `::`.
 * @param[in] symbolTable Symbol table.
 * @return Members named @p callee in the qualifier or one of its ancestors, so `Derived::Inherited(` reaches the
 * base declaration. When the name is a class, its constructors, because `Ns::Type(` constructs a value.
 */
static std::vector<Symbol> ResolveQualifiedCandidates(std::string_view qualifier, std::string_view callee,
                                                      const SymbolTable& symbolTable)
{
    for (const auto& owner : GetInheritedTypeHierarchy(qualifier, symbolTable))
    {
        const auto found = symbolTable.FindMemberSymbolPtr(owner, callee);
        if (!found || found->empty())
        {
            continue;
        }
        if ((*found)[0].type != SymbolType::Class)
        {
            return *found;
        }
        std::vector<Symbol> constructors;
        for (const auto& type : *found)
        {
            const std::string_view typeName =
                type.qualifiedName.empty() ? std::string_view(type.name) : std::string_view(type.qualifiedName);
            for (auto& ctor : symbolTable.FindMemberSymbols(typeName, type.name))
            {
                constructors.push_back(std::move(ctor));
            }
        }
        return constructors.empty() ? *found : constructors;
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
        if (qualifier.empty())
        {
            const auto global = symbolTable.FindSymbolsPtr(callee);
            return global ? *global : std::vector<Symbol>{};
        }
        return ResolveQualifiedCandidates(qualifier, callee, symbolTable);
    }

    const std::string_view prevToken = utils::SliceSource(source, context.prevTokenStartByte, context.prevTokenEndByte);
    return ResolveDirectCandidates(callee, prevToken, scope, symbolTable);
}

} // namespace angel_lsp::analysis
