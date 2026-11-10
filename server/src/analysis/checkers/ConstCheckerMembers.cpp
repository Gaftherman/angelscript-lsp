#include "analysis/checkers/ConstCheckerCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/ScopedNameUtils.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Finds a member variable or property in a class's inheritance hierarchy.
 * @param[in] className Name of the class to search.
 * @param[in] memberName Member name to look for.
 * @param[in] table Workspace symbol table.
 * @param[in,out] cache Optional expression type cache.
 * @return Pointer to VariableSignature if found, nullptr otherwise.
 */
static const VariableSignature* FindClassMemberVariable(std::string_view className, std::string_view memberName,
                                                        const SymbolTable& table, ExpressionTypeCache* cache = nullptr)
{
    if (className.empty() || memberName.empty())
    {
        return nullptr;
    }
    for (const auto& owner : GetInheritedTypeHierarchy(className, table, cache))
    {
        if (const auto candidates = table.FindMemberSymbolPtr(owner, memberName))
        {
            for (const auto& sym : *candidates)
            {
                if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    return &sym.GetVariable();
                }
            }
        }
    }
    return nullptr;
}

/**
 * @brief Checks if an identifier or scoped identifier represents a member of 'this'.
 * @param[in] text Identifier spelling.
 * @param[in] encInfo Enclosing method information.
 * @param[in] ctx Expression type context containing symbols and source.
 * @param[out] outMemberName Resolved member name if matched.
 * @return True if target represents a member of 'this', false otherwise.
 */
static bool IsIdentifierMemberOfThis(std::string_view text, const EnclosingMethodFact* encInfo,
                                     const ExpressionTypeContext& ctx, std::string_view& outMemberName)
{
    const auto [prefix, baseName] = utils::SplitScopedName(text);
    if (!prefix.empty() && prefix != encInfo->className)
    {
        return false;
    }

    if (ctx.scope)
    {
        const Scope* owner = nullptr;
        if (const LocalDefinition* def = ResolveInScope(ctx.scope, baseName, &owner))
        {
            if (def->kind == LocalDefinitionKind::Field || (owner && owner->kind == ScopeKind::Class))
            {
                outMemberName = baseName;
                return true;
            }
            return false;
        }
    }
    if (FindClassMemberVariable(encInfo->className, baseName, ctx.symbolTable, ctx.cache))
    {
        outMemberName = baseName;
        return true;
    }
    return false;
}

bool IsMemberOfThis(const ConstAssignmentTargetFact& target, const ConstSiteFacts& facts,
                    const ExpressionTypeContext& ctx, std::string_view& outMemberName)
{
    const auto* encInfo = facts.FindEnclosingMethodAt(target.bytes.startByte);
    if (!encInfo || !encInfo->inClass || !encInfo->isConstMethod)
    {
        return false;
    }

    if (target.kind == ConstTargetKind::This)
    {
        outMemberName = "this";
        return true;
    }

    if (target.kind == ConstTargetKind::ThisMember)
    {
        outMemberName = target.memberName;
        return true;
    }

    if (target.kind == ConstTargetKind::Identifier)
    {
        return IsIdentifierMemberOfThis(target.actualTargetText, encInfo, ctx, outMemberName);
    }

    if (target.kind == ConstTargetKind::Member)
    {
        if (target.receiverText == "this")
        {
            outMemberName = target.memberName;
            return true;
        }

        if (target.leafIsThis)
        {
            bool hasHandleType = false;
            if (!target.leafText.empty() && target.leafText != "this")
            {
                if (const auto* var =
                        FindClassMemberVariable(encInfo->className, target.leafText, ctx.symbolTable, ctx.cache))
                {
                    hasHandleType = var->modifiers.isHandle;
                }
                else if (ctx.scope)
                {
                    if (const auto* def = ResolveInScope(ctx.scope, target.leafText))
                    {
                        hasHandleType = def->isHandleType;
                    }
                }
            }
            if (!hasHandleType)
            {
                outMemberName = target.memberName;
                return true;
            }
        }
    }

    return false;
}

} // namespace angel_lsp::analysis
