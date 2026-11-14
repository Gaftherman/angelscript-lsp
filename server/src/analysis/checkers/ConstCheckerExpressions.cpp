#include "analysis/checkers/ConstCheckerCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <optional>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a declared type's written form begins with const qualifier.
 * @param[in] typeText Type name string view to inspect.
 * @return True if typeText starts with const, false otherwise.
 */
static bool TypeTextIsConst(std::string_view typeText)
{
    return HasLeadingConst(typeText);
}

/**
 * @brief Checks if a handle type is declared const itself (e.g. Type@ const).
 * @param[in] typeText Type name string view to inspect.
 * @return True if handle type has trailing const, false otherwise.
 */
static bool TypeTextIsHandleConst(std::string_view typeText)
{
    return HasTrailingConst(typeText);
}

/**
 * @brief Resolves the constness of a plain identifier name across local and global scopes.
 * @param[in] name Identifier name of the variable or property.
 * @param[in] byteOffset Start byte offset for scope lookup.
 * @param[in] facts Document-wide const site facts.
 * @param[in] ctx Expression type context containing scope and symbol table.
 * @return Resolved Constness enum value.
 */
static Constness ResolveNameConstness(std::string_view name, uint32_t byteOffset, const ConstSiteFacts& facts,
                                      const ExpressionTypeContext& ctx)
{
    if (name.empty())
    {
        return Constness::Unknown;
    }

    if (ctx.scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(ctx.scope, name, byteOffset))
        {
            if (!def->typeName.empty())
            {
                return TypeTextIsConst(def->typeName) ? Constness::Const : Constness::Mutable;
            }

            if (const auto* param = facts.FindParameter(byteOffset, name))
            {
                return param->isConst ? Constness::Const : Constness::Mutable;
            }
            return Constness::Unknown;
        }
    }

    const auto symbols = ctx.symbolTable.FindSymbolsPtr(name);
    if (!symbols)
    {
        return Constness::Unknown;
    }

    for (const auto& sym : *symbols)
    {
        if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
        {
            return sym.GetVariable().modifiers.isConst ? Constness::Const : Constness::Mutable;
        }
    }
    return Constness::Unknown;
}

/**
 * @brief Resolves the constness of a leaf expression (identifier or this).
 * @param[in] leaf Leaf details of a member expression chain.
 * @param[in] facts Document-wide const site facts.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
static Constness ResolveLeafConstness(const ConstChainLeafFact& leaf, const ConstSiteFacts& facts,
                                      const ExpressionTypeContext& ctx)
{
    if (leaf.leafIsThis || leaf.leafText == "this")
    {
        const auto* encInfo = facts.FindEnclosingMethodAt(leaf.leafBytes.startByte);
        return (encInfo && encInfo->inClass && encInfo->isConstMethod) ? Constness::Const : Constness::Mutable;
    }

    if (!leaf.leafText.empty())
    {
        const Constness qualified = ResolveNameConstness(leaf.leafText, leaf.leafBytes.startByte, facts, ctx);
        if (qualified != Constness::Unknown)
        {
            return qualified;
        }
        return ResolveNameConstness(LastScopeSegment(leaf.leafText), leaf.leafBytes.startByte, facts, ctx);
    }

    return Constness::Unknown;
}

/**
 * @brief Resolves constness from a member chain leaf fact.
 * @param[in] leaf Leaf details of a member chain.
 * @param[in] facts Document-wide const site facts.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
static Constness ResolveConstnessFromLeaf(const ConstChainLeafFact& leaf, const ConstSiteFacts& facts,
                                          const ExpressionTypeContext& ctx)
{
    const Constness leafConst = ResolveLeafConstness(leaf, facts, ctx);
    if (leaf.inMemberChain)
    {
        return leafConst == Constness::Const ? Constness::Const : Constness::Unknown;
    }
    return leafConst;
}

/**
 * @brief Resolves whether the target of an assignment is const at its leaf.
 * @param[in] target Assignment-target fact carrying the leaf byte span, text and member-chain flags.
 * @param[in] facts Const-site facts of the document being checked.
 * @param[in] ctx Expression type context used to resolve the leaf's declared type.
 * @return Constness of the leaf, Unknown when it cannot be resolved.
 */
Constness ResolveConstness(const ConstAssignmentTargetFact& target, const ConstSiteFacts& facts,
                           const ExpressionTypeContext& ctx)
{
    const ConstChainLeafFact leaf{target.leafBytes, target.leafText, target.leafIsThis, target.inMemberChain};
    return ResolveConstnessFromLeaf(leaf, facts, ctx);
}

/**
 * @brief Resolves whether the receiver of a method call is const at its leaf.
 * @param[in] call Method-call fact carrying the leaf byte span, text and member-chain flags.
 * @param[in] facts Const-site facts of the document being checked.
 * @param[in] ctx Expression type context used to resolve the leaf's declared type.
 * @return Constness of the leaf, Unknown when it cannot be resolved.
 */
Constness ResolveConstness(const ConstMethodCallFact& call, const ConstSiteFacts& facts,
                           const ExpressionTypeContext& ctx)
{
    const ConstChainLeafFact leaf{call.leafBytes, call.leafText, call.leafIsThis, call.inMemberChain};
    return ResolveConstnessFromLeaf(leaf, facts, ctx);
}

Constness ResolveHandleConstness(std::string_view name, uint32_t startByte, const ConstSiteFacts& facts,
                                 const ExpressionTypeContext& ctx)
{
    if (name.empty())
    {
        return Constness::Unknown;
    }

    if (ctx.scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(ctx.scope, LastScopeSegment(name), startByte))
        {
            if (!def->typeName.empty())
            {
                return TypeTextIsHandleConst(def->typeName) ? Constness::Const : Constness::Mutable;
            }
            if (const auto* param = facts.FindParameter(startByte, name))
            {
                return param->isHandleConst ? Constness::Const : Constness::Mutable;
            }
        }
    }

    if (const auto symbols = ctx.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
            {
                return TypeTextIsHandleConst(sym.GetVariable().typeName) ? Constness::Const : Constness::Mutable;
            }
        }
    }
    return Constness::Unknown;
}

} // namespace angel_lsp::analysis
