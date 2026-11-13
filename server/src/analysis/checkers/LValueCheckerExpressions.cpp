#include "analysis/checkers/LValueChecker.h"
#include "analysis/checkers/LValueCheckerCommon.h"
#include "analysis/index/LValueSiteFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/AccessorNames.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if an identifier or symbol name denotes an assignable variable or property.
 * @param[in] name Identifier name to inspect.
 * @param[in] scope Current lexical scope.
 * @param[in] table Workspace symbol table.
 * @param[in] refByte Byte offset of the reference node in source code.
 * @return True if identifier can be assigned to; false otherwise.
 */
static bool IsIdentifierAssignable(std::string_view name, const Scope* scope, const SymbolTable& table,
                                   uint32_t refByte = UINT32_MAX)
{
    if (IsPrimitiveTypeName(name))
    {
        return false;
    }

    if (scope)
    {
        const LocalDefinition* localDef = ResolveInScopeAt(scope, name, refByte);
        if (localDef)
        {
            return localDef->kind != LocalDefinitionKind::Function && localDef->kind != LocalDefinitionKind::Method;
        }
    }

    auto symbols = table.FindSymbolsPtr(name);
    if (symbols && !symbols->empty())
    {
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
            {
                return true;
            }
        }
        return false;
    }
    return true;
}

/**
 * @brief Checks if a member expression denotes an assignable property, variable, or setter method.
 * @param[in] candidate Member candidate fact.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context containing symbol table and expression cache.
 * @return True if member expression can be assigned to; false otherwise.
 */
static bool IsAssignableMember(const LValueCandidateFact& candidate, const Scope* scope, const DiagnosticContext& ctx)
{
    if (candidate.memberName.empty() || candidate.objectBytes.endByte == 0)
    {
        return true;
    }
    const std::string objType = ResolveExpressionType(candidate.objectBytes, ExpressionTypeContext(scope, ctx));
    const auto cleanObj = CleanBaseType(objType);
    if (cleanObj.empty())
    {
        return true;
    }
    for (const auto& typeName : GetInheritedTypeHierarchy(cleanObj, ctx.request.symbolTable, ctx.request.exprCache))
    {
        if (auto found = ctx.request.symbolTable.FindMemberSymbolPtr(typeName, candidate.memberName))
        {
            bool hasAssignable = false;
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    hasAssignable = true;
                    break;
                }
            }
            if (!hasAssignable)
            {
                bool hasSetter = false;
                for (const auto& ancestor :
                     GetInheritedTypeHierarchy(cleanObj, ctx.request.symbolTable, ctx.request.exprCache))
                {
                    char setterBuf[512];
                    const std::string_view setterName =
                        utils::ComposeAccessorName(utils::k_setterPrefix, candidate.memberName, setterBuf);
                    auto setters = setterName.empty()
                                       ? nullptr
                                       : ctx.request.symbolTable.FindMemberSymbolPtr(ancestor, setterName);
                    if (setters && !setters->empty())
                    {
                        hasSetter = true;
                        break;
                    }
                }
                if (!hasSetter)
                {
                    return false;
                }
            }
        }
    }
    return true;
}

/**
 * @brief Checks if both branches of a ternary expression are assignable l-values of matching type.
 * @param[in] candidate Ternary candidate fact.
 * @param[in] facts Document-wide l-value facts for branch candidate lookup.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context containing symbol table and expression cache.
 * @return True if ternary expression can be assigned to; false otherwise.
 */
static bool IsTernaryAssignable(const LValueCandidateFact& candidate, const LValueSiteFacts& facts, const Scope* scope,
                                const DiagnosticContext& ctx)
{
    const auto* consCand = facts.FindCandidate(candidate.consequenceBytes);
    const auto* altCand = facts.FindCandidate(candidate.alternativeBytes);
    if (!consCand || !altCand)
    {
        return false;
    }
    if (!LValueNodeValidator::IsAssignableLValueNode(*consCand, facts, scope, ctx) ||
        !LValueNodeValidator::IsAssignableLValueNode(*altCand, facts, scope, ctx))
    {
        return false;
    }
    const std::string type1 = ResolveExpressionType(candidate.consequenceBytes, ExpressionTypeContext(scope, ctx));
    const std::string type2 = ResolveExpressionType(candidate.alternativeBytes, ExpressionTypeContext(scope, ctx));
    if (!type1.empty() && !type2.empty() && CleanBaseType(type1) != CleanBaseType(type2))
    {
        return false;
    }
    return true;
}

/**
 * @brief Checks if a unary expression is an assignable handle dereference expression.
 * @param[in] candidate Unary candidate fact.
 * @param[in] facts Document-wide l-value facts.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context containing symbol table and expression cache.
 * @return True if unary operand is an assignable l-value; false otherwise.
 */
static bool IsUnaryAssignable(const LValueCandidateFact& candidate, const LValueSiteFacts& facts, const Scope* scope,
                              const DiagnosticContext& ctx)
{
    if (candidate.kind != LValueCandidateKind::UnaryDereference)
    {
        return false;
    }
    const auto* operand = facts.FindCandidate(candidate.operandBytes);
    if (!operand)
    {
        return false;
    }
    return LValueNodeValidator::IsAssignableLValueNode(*operand, facts, scope, ctx);
}

bool LValueNodeValidator::IsAssignableLValueNode(const LValueCandidateFact& candidate, const LValueSiteFacts& facts,
                                                 const Scope* scope, const DiagnosticContext& ctx)
{
    switch (candidate.kind)
    {
    case LValueCandidateKind::Ternary:
        return IsTernaryAssignable(candidate, facts, scope, ctx);
    case LValueCandidateKind::UnaryDereference:
        return IsUnaryAssignable(candidate, facts, scope, ctx);
    case LValueCandidateKind::Member:
        return IsAssignableMember(candidate, scope, ctx);
    case LValueCandidateKind::Index:
        return true;
    case LValueCandidateKind::Identifier:
        return IsIdentifierAssignable(candidate.name, scope, ctx.request.symbolTable, candidate.refByte);
    default:
        return false;
    }
}

} // namespace angel_lsp::analysis
