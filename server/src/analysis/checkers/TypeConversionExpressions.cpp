#include "analysis/checkers/BinaryOperatorChecker.h"
#include "analysis/checkers/ComparisonOperatorChecker.h"
#include "analysis/checkers/HandleComparisonChecker.h"
#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Inspects the accessor metadata of an expression node that may name a virtual property.
 * @param[in] exprNode Expression node accessing a property.
 * @param[in] scope Enclosing lexical scope.
 * @param[in] ctx Diagnostic context.
 * @return PropertyAccessInfo structure; no flag is set for an expression that names no property.
 */
static PropertyAccessInfo InspectNodePropertyAccess(TSNode exprNode, const Scope* scope, const DiagnosticContext& ctx)
{
    return InspectPropertyAccess(ReadPropertyAccess(exprNode, ctx.request.nodeIndex, ctx.request.sourceCode), scope,
                                 ctx);
}

void ProcessBinaryNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const BinaryExpressionFact* fact =
        request.binaryFacts->Find(IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)});
    if (!fact)
    {
        return;
    }
    const Scope* scope = ResolveNodeScope(node, request);

    if (fact->opClass == BinaryOperatorClass::Identity)
    {
        CheckHandleComparison(*fact, scope, ctx);
        return;
    }

    if (fact->opClass == BinaryOperatorClass::Equality || fact->opClass == BinaryOperatorClass::Relational)
    {
        CheckSignedUnsignedComparison(*fact, scope, ctx);
        CheckHandleComparison(*fact, scope, ctx);
        CheckComparisonOperatorCompatibility(*fact, scope, ctx);
        return;
    }

    CheckBinaryOperatorCompatibility(*fact, scope, ctx);
}

static TSNode FindUnaryOperand(TSNode node, const NodeIndex* index)
{
    const SyntaxOperands syntax(node, index);
    const auto& gs = parser::GetGrammarSymbols();
    TSNode operand = syntax.Field(gs.fieldOperand);
    if (!ts_node_is_null(operand))
    {
        return operand;
    }
    const TSNode opNode = syntax.Field(gs.fieldOperator);
    TSNode found{};
    syntax.ForEachNamed(
        [&](TSNode child)
        {
            if (!ts_node_is_null(opNode) && ts_node_eq(child, opNode))
            {
                return true;
            }
            const TSSymbol sym = ts_node_symbol(child);
            if (sym == gs.tokPlusPlus || sym == gs.tokDashDash || sym == gs.tokBang || sym == gs.tokNot ||
                sym == gs.tokDash || sym == gs.tokPlus || sym == gs.tokTilde)
            {
                return true;
            }
            found = child;
            return false;
        });
    return found;
}

static bool CheckUnaryInvalidOperand(TSNode node, TSNode operand, const Scope* scope, DiagnosticContext& ctx)
{
    if (ts_node_is_null(operand))
    {
        return false;
    }
    if (const auto dt = IsBareDataType(operand, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(operand, ctx, *dt);
        return true;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = SyntaxOperands(node, ctx.request.nodeIndex).Field(gs.fieldOperator);
    if (!ts_node_is_null(opNode) && ts_node_symbol(opNode) == gs.tokAt)
    {
        return false;
    }
    if (IsFunctionReference(operand, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(node), diagnostics::codes::IllegalOperation);
        return true;
    }
    return false;
}

void ProcessUnaryOrPostfixNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(node, request);
    TSNode operand = FindUnaryOperand(node, ctx.request.nodeIndex);
    if (CheckUnaryInvalidOperand(node, operand, scope, ctx))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = SyntaxOperands(node, ctx.request.nodeIndex).Field(gs.fieldOperator);
    const TSSymbol opSym = ts_node_symbol(opNode);
    if (opSym != gs.tokPlusPlus && opSym != gs.tokDashDash)
    {
        return;
    }

    if (!ts_node_is_null(operand))
    {
        TSNode unwrapped = UnwrapIndexedParentheses(operand, ctx.request.nodeIndex);
        if (ts_node_symbol(unwrapped) == gs.symIndexExpression)
        {
            TSNode arrayNode = SyntaxOperands(unwrapped, ctx.request.nodeIndex).FieldOrNamed(gs.fieldObject);
            PropertyAccessInfo pInfo = InspectNodePropertyAccess(arrayNode, scope, ctx);
            if (pInfo.isProperty && pInfo.isIndexed)
            {
                ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-inc-dec-on-virtual-prop", pInfo.propName);
                return;
            }
        }
        else
        {
            PropertyAccessInfo pInfo = InspectNodePropertyAccess(unwrapped, scope, ctx);
            if (pInfo.isProperty)
            {
                ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-inc-dec-on-virtual-prop", pInfo.propName);
                return;
            }
        }
    }
}

static bool IsWriteOrMutationOperand(const IndexedSyntaxNode& fact)
{
    const auto& gs = parser::GetGrammarSymbols();
    return (fact.parentSymbol == gs.symAssignmentExpression && fact.field == gs.fieldLeft) ||
           fact.parentSymbol == gs.symUnaryExpression || fact.parentSymbol == gs.symPostfixExpression;
}

void ProcessMemberExpressionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto* fact = request.nodeIndex.FindOperandSyntax(node);
    if (fact && fact->parentSymbol == gs.symCallExpression && fact->field == gs.fieldFunction)
    {
        return;
    }

    if (!fact || !IsWriteOrMutationOperand(*fact))
    {
        const Scope* scope = ResolveNodeScope(node, request);
        PropertyAccessInfo pInfo = InspectNodePropertyAccess(node, scope, ctx);
        if (pInfo.isProperty && !pInfo.hasGet && pInfo.hasSet)
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-write-only-property", pInfo.propName);
        }
    }
}

void ProcessIndexExpressionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto* fact = request.nodeIndex.FindOperandSyntax(node);
    if (!fact || !IsWriteOrMutationOperand(*fact))
    {
        const Scope* scope = ResolveNodeScope(node, request);
        TSNode arrayNode =
            SyntaxOperands(node, ctx.request.nodeIndex).FieldOrNamed(parser::GetGrammarSymbols().fieldObject);
        PropertyAccessInfo pInfo = InspectNodePropertyAccess(arrayNode, scope, ctx);
        if (pInfo.isProperty && pInfo.isIndexed && !pInfo.hasGet && pInfo.hasSet)
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-write-only-property", pInfo.propName);
        }
    }
}

static bool CheckCastVisibilityGuards(const ConversionTypePair& types, bool sourceIsScalar, bool targetIsScalar,
                                      const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (!targetIsScalar && !FindTypeDeclaration(types.to, table).found)
    {
        return false;
    }
    if (!sourceIsScalar && !FindTypeDeclaration(types.from, table).found)
    {
        return false;
    }
    if (ctx.request.IsRegisteredSymbol(types.from) || ctx.request.IsRegisteredSymbol(types.to))
    {
        return false;
    }
    return true;
}

static void CheckCast(TSNode castNode, const Scope* scope, DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(castNode, ctx.request.nodeIndex);
    const std::string targetName =
        CleanBaseType(parser::NodeText(syntax.Field(parser::GetGrammarSymbols().fieldType), ctx.request.sourceCode));

    TSNode valueNode = syntax.Field(parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(valueNode) || targetName.empty())
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const bool targetIsScalar = IsScalarCastTarget(targetName, table);

    const ExpressionType source = ResolveValueType(valueNode, scope, ctx);
    if (!source.known || source.baseName.empty() || IsSameType(source.baseName, targetName))
    {
        return;
    }

    const bool sourceIsScalar = IsScalarCastTarget(source.baseName, table);
    if (!CheckCastVisibilityGuards({source.baseName, targetName}, sourceIsScalar, targetIsScalar, ctx))
    {
        return;
    }

    if (AreHierarchyRelated(source.baseName, targetName, table))
    {
        return;
    }

    if (!sourceIsScalar && !targetIsScalar)
    {
        return;
    }

    if (DeclaresAnyCastOperator(source.baseName, table) || DeclaresAnyCastOperator(targetName, table))
    {
        return;
    }

    ctx.EmitAtNodeRange(ToOptionalSourceRange(castNode), "as-err-invalid-cast", {source.baseName, targetName});
}

void ProcessCastNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(node, request);
    CheckCast(node, scope, ctx);
}

} // namespace angel_lsp::analysis
