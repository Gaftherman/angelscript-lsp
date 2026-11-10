#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/BinaryExpressionSyntax.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

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
    if (scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(scope, LastScopeSegment(identifier), byte))
        {
            return def->isConst || IsConstTypeView(def->typeName);
        }
    }

    bool folded = false;
    ForEachSymbolNamed(identifier, ctx.request.symbolTable,
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

/**
 * @brief Warns when an indexed float operand implicitly converts to an integer type.
 * @param[in] value Converted operand with its constant-folding data.
 * @param[in] types Source and destination types.
 * @param[in] scope Enclosing lexical scope used to fold constant identifiers.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckFloatTruncation(const BinaryOperandFact& value, const ConversionTypePair& types, const Scope* scope,
                          DiagnosticContext& ctx)
{
    if (!IsFloatingPointPrimitive(types.from) || !IsIntegerPrimitive(types.to))
    {
        return;
    }
    if (IsFoldedConstantOperand(value, scope, ctx))
    {
        return;
    }
    ctx.EmitAtRange(value.range, "as-warn-float-truncation", {types.from, types.to}, DiagnosticSeverity::Warning);
}

} // namespace angel_lsp::analysis
