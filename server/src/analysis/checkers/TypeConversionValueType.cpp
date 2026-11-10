#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Classifies a numeric literal as integral or floating point.
 * @param[in] text Literal spelling.
 * @return "float" when the spelling is decimal and carries a point, exponent or suffix; "int" otherwise.
 */
static std::string_view ClassifyNumberLiteral(std::string_view text)
{
    const bool isHex = text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (!isHex)
    {
        for (const char c : text)
        {
            if (c == '.' || c == 'e' || c == 'E' || c == 'f' || c == 'F')
            {
                return "float";
            }
        }
    }
    return "int";
}

/**
 * @brief Resolves the type of `this` from the class or interface that encloses the expression.
 * @param[in] value Identifier fact of the `this` expression.
 * @param[in] ctx Diagnostic context.
 * @return The enclosing class type, or an unknown type outside any class.
 */
static ExpressionType ResolveThisValueType(const ValueExpressionFact& value, const DiagnosticContext& ctx)
{
    if (!ctx.request.nodeIndex)
    {
        return {};
    }
    const auto containers = ctx.request.nodeIndex->CollectLookupContainers(value.bytes.startByte, value.bytes.endByte,
                                                                           value.nodeSymbol, ctx.request.sourceCode);
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            return ExpressionType{container.name, true, false};
        }
    }
    return {};
}

/**
 * @brief Resolves a variable or property name through the local scope, then the symbol table.
 * @param[in] value Identifier fact.
 * @param[in] scope Lexical scope at the expression position.
 * @param[in] ctx Diagnostic context.
 * @return The declared type, or an unknown type when no typed variable matches.
 */
static ExpressionType ResolveIdentifierValueType(const ValueExpressionFact& value, const Scope* scope,
                                                 const DiagnosticContext& ctx)
{
    if (value.text == "this")
    {
        return ResolveThisValueType(value, ctx);
    }
    if (scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(scope, value.text, value.bytes.startByte);
            def && !def->typeName.empty())
        {
            const bool isConst = def->isConst || IsConstTypeView(def->typeName);
            return ExpressionType{CleanBaseType(def->typeName), true, false, isConst};
        }
    }
    ExpressionType result;
    ForEachSymbolNamed(value.text, ctx.request.symbolTable,
                       [&result](const Symbol& sym)
                       {
                           if (sym.type != SymbolType::Variable && sym.type != SymbolType::Property)
                           {
                               return true;
                           }
                           if (!sym.GetVariable().typeName.empty())
                           {
                               const auto& var = sym.GetVariable();
                               const bool isConst = var.modifiers.isConst || IsConstTypeView(var.typeName);
                               result = ExpressionType{CleanBaseType(var.typeName), true, false, isConst};
                               return true;
                           }
                           return false;
                       });
    return result;
}

/**
 * @brief Resolves the result type of a call or construct call from its callee spelling.
 * @param[in] value Call fact whose text is the callee.
 * @param[in] ctx Diagnostic context.
 * @return The constructed type or the return type of the first matching function.
 */
static ExpressionType ResolveCallValueType(const ValueExpressionFact& value, const DiagnosticContext& ctx)
{
    const auto calleeName = CleanBaseType(value.text);
    if (calleeName.empty())
    {
        return {};
    }
    if (FindTypeDeclaration(calleeName, ctx.request.symbolTable).found || IsBuiltInValueType(calleeName, ctx))
    {
        return ExpressionType{calleeName, true, false, false};
    }
    ExpressionType result;
    ForEachSymbolNamed(LastScopeSegment(calleeName), ctx.request.symbolTable,
                       [&result](const Symbol& sym)
                       {
                           if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                           {
                               const auto& fn = sym.GetFunction();
                               result = ExpressionType{CleanBaseType(fn.returnType), true, false, fn.returnIsConst};
                               return true;
                           }
                           return false;
                       });
    return result;
}

/**
 * @brief Resolves the type of an expression whose leaf is a literal.
 * @param[in] value Literal fact.
 * @param[in] ctx Diagnostic context supplying the effective string type name.
 * @return The exact literal type; unknown for any other leaf.
 */
static ExpressionType ResolveLiteralValueType(const ValueExpressionFact& value, const DiagnosticContext& ctx)
{
    ExpressionType result;
    result.known = true;
    result.isLiteral = true;
    switch (value.kind)
    {
    case ValueExpressionKind::Number:
        result.baseName = ClassifyNumberLiteral(value.text);
        break;
    case ValueExpressionKind::String:
        result.baseName = ctx.request.GetEffectiveStringTypeName();
        break;
    case ValueExpressionKind::Boolean:
        result.baseName = "bool";
        break;
    default:
        return {};
    }
    return result;
}

/**
 * @brief Resolves the type of an explicit cast from its written target type.
 * @param[in] value Cast fact whose text is the target type.
 * @return The cleaned target type with its constness, or unknown when the type is empty.
 */
static ExpressionType ResolveCastValueType(const ValueExpressionFact& value)
{
    const auto qualifiers = ExtractTypeQualifiersView(value.text);
    const auto typeText = CleanBaseType(value.text);
    return typeText.empty() ? ExpressionType{} : ExpressionType{typeText, true, false, qualifiers.isConst};
}

/**
 * @brief Resolves the type of a member expression through byte-range expression resolution.
 * @param[in] value Member fact.
 * @param[in] scope Lexical scope at the expression position.
 * @param[in] ctx Diagnostic context.
 * @return The resolved member type, or unknown when it cannot be resolved.
 */
static ExpressionType ResolveMemberValueType(const ValueExpressionFact& value, const Scope* scope,
                                             const DiagnosticContext& ctx)
{
    const auto resolved = ResolveExpressionType(value.bytes, ExpressionTypeContext(scope, ctx));
    const auto qualifiers = ExtractTypeQualifiersView(resolved);
    return resolved.empty() ? ExpressionType{}
                            : ExpressionType{CleanBaseType(resolved), true, false, qualifiers.isConst};
}

/**
 * @brief Swaps an unsigned base type for the signed type of the same width.
 * @param[in,out] type Type to rewrite; other base names are left untouched.
 */
static void NegateUnsignedType(ExpressionType& type)
{
    if (type.baseName == "uint" || type.baseName == "uint32")
    {
        type.baseName = "int";
    }
    else if (type.baseName == "uint8")
    {
        type.baseName = "int8";
    }
    else if (type.baseName == "uint16")
    {
        type.baseName = "int16";
    }
    else if (type.baseName == "uint64")
    {
        type.baseName = "int64";
    }
}

ExpressionType ResolveValueType(const ValueExpressionFact& value, const Scope* scope, const DiagnosticContext& ctx)
{
    if (value.logicalNot)
    {
        return ExpressionType{"bool", true, false};
    }

    ExpressionType result;
    switch (value.kind)
    {
    case ValueExpressionKind::Poison:
        result.baseName = ExpressionTypeCache::k_poisonType;
        break;
    case ValueExpressionKind::Number:
    case ValueExpressionKind::String:
    case ValueExpressionKind::Boolean:
        result = ResolveLiteralValueType(value, ctx);
        break;
    case ValueExpressionKind::Identifier:
        result = ResolveIdentifierValueType(value, scope, ctx);
        break;
    case ValueExpressionKind::Call:
        result = ResolveCallValueType(value, ctx);
        break;
    case ValueExpressionKind::Cast:
        result = ResolveCastValueType(value);
        break;
    case ValueExpressionKind::Member:
        result = ResolveMemberValueType(value, scope, ctx);
        break;
    case ValueExpressionKind::None:
    case ValueExpressionKind::Null:
        break;
    }
    if (value.negated)
    {
        NegateUnsignedType(result);
    }
    return result;
}

} // namespace angel_lsp::analysis
