#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/BinaryExpressionSyntax.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/OperandRangeResolution.h"
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
    ctx.EmitAtNodeRange(ToOptionalSourceRange(node), diagnostics::codes::ExpressionIsDataType, dataTypeName);
}

void EmitExpressionIsDataType(const ConversionExpressionFact& expression, const DiagnosticContext& ctx,
                              std::string_view dataTypeName)
{
    const auto* index = ctx.request.nodeIndex;
    if (index && (index->HasSyntaxErrors() || index->IsInsideConditionalStatement(expression.bytes.startByte)))
    {
        return;
    }
    ctx.EmitAtRange(expression.range, diagnostics::codes::ExpressionIsDataType, dataTypeName);
}

const Scope* ResolveNodeScope(TSNode node, const TypeConversionCheckRequest& request)
{
    const TSPoint start = ts_node_start_point(node);
    return FindInnermostScope(request.scopeRoot, start.row, start.column);
}

const Scope* ResolveNodeScope(const SourceRange& at, const TypeConversionCheckRequest& request)
{
    return FindInnermostScope(request.scopeRoot, at.startLine, at.startCharacter);
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

static bool IsConstantIdentifier(std::string_view identifier, uint32_t byte, const Scope* scope,
                                 const DiagnosticContext& ctx)
{
    const std::string name(identifier);
    if (scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(scope, LastScopeSegment(name), byte))
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

    return IsConstantIdentifier(parser::NodeText(current, ctx.request.sourceCode), ts_node_start_byte(current), scope,
                                ctx);
}

static bool IsFoldedConstantOperand(const BinaryOperandFact& operand, const Scope* scope, const DiagnosticContext& ctx)
{
    if (operand.isNumericLiteral)
    {
        return true;
    }
    return !operand.foldedName.empty() && IsConstantIdentifier(operand.foldedName, operand.foldedByte, scope, ctx);
}

void CheckSignedUnsignedComparison(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx)
{
    const std::string leftType =
        CleanBaseType(ResolveExpressionType(fact.left.bytes, ExpressionTypeContext(scope, ctx)));
    const std::string rightType =
        CleanBaseType(ResolveExpressionType(fact.right.bytes, ExpressionTypeContext(scope, ctx)));

    const bool mismatched = (IsUnsignedIntegerPrimitive(leftType) && IsSignedNumericPrimitive(rightType)) ||
                            (IsUnsignedIntegerPrimitive(rightType) && IsSignedNumericPrimitive(leftType));
    if (!mismatched)
    {
        return;
    }

    if (IsFoldedConstantOperand(fact.left, scope, ctx) || IsFoldedConstantOperand(fact.right, scope, ctx))
    {
        return;
    }

    ctx.EmitAtRange(fact.operatorRange, "as-warn-signed-unsigned-mismatch", {leftType, rightType},
                    DiagnosticSeverity::Warning);
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
    ctx.EmitAtNodeRange(ToOptionalSourceRange(valueNode), "as-warn-float-truncation", {types.from, types.to},
                        DiagnosticSeverity::Warning);
}

} // namespace angel_lsp::analysis
