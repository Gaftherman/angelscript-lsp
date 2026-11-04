#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

void EmitExpressionIsDataType(TSNode node, const DiagnosticContext& ctx, std::string_view dataTypeName)
{
    const auto* index = ctx.request.nodeIndex;
    if (index && (index->HasSyntaxErrors() || index->IsInsideConditionalStatement(ts_node_start_byte(node))))
    {
        return;
    }
    ctx.EmitAtNode(node, diagnostics::codes::ExpressionIsDataType, dataTypeName);
}

static bool IsUnsignedIntegerPrimitive(std::string_view typeName) noexcept
{
    return typeName == "uint" || typeName == "uint8" || typeName == "uint16" || typeName == "uint32" ||
           typeName == "uint64";
}

static bool IsSignedNumericPrimitive(std::string_view typeName) noexcept
{
    return typeName == "int" || typeName == "int8" || typeName == "int16" || typeName == "int32" ||
           typeName == "int64" || typeName == "float" || typeName == "double";
}

static bool IsNumericLiteralExpression(TSNode node, const NodeIndex* index)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    while (!ts_node_is_null(current))
    {
        const TSSymbol sym = ts_node_symbol(current);
        if (sym == gs.symNumberLiteral || sym == gs.symStringLiteral || sym == gs.symBooleanLiteral ||
            sym == gs.symNullLiteral)
        {
            return true;
        }
        if (sym != gs.symParenthesizedExpression && sym != gs.symUnaryExpression)
        {
            return false;
        }
        current = SyntaxOperands(current, index).FirstNamed();
        if (ts_node_is_null(current))
        {
            return false;
        }
    }
    return false;
}

static bool IsFoldedConstantOperand(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    while (!ts_node_is_null(current))
    {
        if (IsNumericLiteralExpression(current, ctx.request.nodeIndex))
        {
            return true;
        }

        if (ts_node_symbol(current) == gs.symParenthesizedExpression)
        {
            current = SyntaxOperands(current, ctx.request.nodeIndex).FirstNamed();
            if (ts_node_is_null(current))
            {
                return false;
            }
            continue;
        }

        break;
    }

    if (ts_node_is_null(current))
    {
        return false;
    }

    const TSSymbol sym = ts_node_symbol(current);
    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return false;
    }

    const std::string name = parser::NodeText(current, ctx.request.sourceCode);
    if (scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(scope, LastScopeSegment(name), ts_node_start_byte(current)))
        {
            return def->isConst || IsConstTypeView(def->typeName);
        }
    }

    bool folded = false;
    ForEachSymbolNamed(name, ctx.request.symbolTable,
                       [&folded](const Symbol& symVal)
                       {
                           if (symVal.type != SymbolType::Variable && symVal.type != SymbolType::Property)
                           {
                               return false;
                           }
                           folded =
                               symVal.GetVariable().modifiers.isConst || IsConstTypeView(symVal.GetVariable().typeName);
                           return true;
                       });
    return folded;
}

void CheckSignedUnsignedComparison(const BinaryExpressionOperands& operands, const Scope* scope, DiagnosticContext& ctx)
{
    const auto& [opNode, left, right] = operands;
    const std::string leftType = CleanBaseType(ResolveExpressionType(left, ExpressionTypeContext(scope, ctx)));
    const std::string rightType = CleanBaseType(ResolveExpressionType(right, ExpressionTypeContext(scope, ctx)));

    const bool mismatched = (IsUnsignedIntegerPrimitive(leftType) && IsSignedNumericPrimitive(rightType)) ||
                            (IsUnsignedIntegerPrimitive(rightType) && IsSignedNumericPrimitive(leftType));
    if (!mismatched)
    {
        return;
    }

    if (IsFoldedConstantOperand(left, scope, ctx) || IsFoldedConstantOperand(right, scope, ctx))
    {
        return;
    }

    ctx.EmitAtNode(opNode, "as-warn-signed-unsigned-mismatch", {leftType, rightType}, DiagnosticSeverity::Warning);
}

void CheckFloatTruncation(TSNode valueNode, const ConversionTypePair& types, const Scope* scope, DiagnosticContext& ctx)
{
    if (ts_node_is_null(valueNode))
    {
        return;
    }
    if (!IsFloatingPointPrimitive(types.from) || !IsIntegerPrimitive(types.to))
    {
        return;
    }
    if (IsFoldedConstantOperand(valueNode, scope, ctx))
    {
        return;
    }
    ctx.EmitAtNode(valueNode, "as-warn-float-truncation", {types.from, types.to}, DiagnosticSeverity::Warning);
}

} // namespace angel_lsp::analysis
