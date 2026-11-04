#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Classifies a numeric literal as integral or floating point.
 */
static std::string ClassifyNumberLiteral(std::string_view text)
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
 * @brief Finds the class a 'this' expression refers to at a given node.
 */
static std::string EnclosingClassName(TSNode node, std::string_view sourceCode, const NodeIndex* nodeIndex)
{
    for (const auto& container : GetEnclosingContainers(node, sourceCode, nodeIndex))
    {
        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            return container.name;
        }
    }
    return "";
}

ExpressionType ResolveIdentifierValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    const std::string name = parser::NodeText(node, ctx.request.sourceCode);
    if (name == "this")
    {
        const std::string className = EnclosingClassName(node, ctx.request.sourceCode, ctx.request.nodeIndex);
        return className.empty() ? ExpressionType{} : ExpressionType{className, true, false};
    }

    if (scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(scope, name, ts_node_start_byte(node));
            def && !def->typeName.empty())
        {
            const bool isConst = def->isConst || IsConstTypeView(def->typeName);
            return ExpressionType{CleanBaseType(def->typeName), true, false, isConst};
        }
    }

    ExpressionType result;
    ForEachSymbolNamed(name, ctx.request.symbolTable,
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

ExpressionType ResolveCallValueType(TSNode node, const DiagnosticContext& ctx)
{
    TSNode callee = SyntaxOperands(node, ctx.request.nodeIndex).Callee();
    if (ts_node_is_null(callee))
    {
        return ExpressionType{};
    }

    const std::string calleeName = CleanBaseType(parser::NodeText(callee, ctx.request.sourceCode));
    if (calleeName.empty())
    {
        return ExpressionType{};
    }

    if (FindTypeDeclaration(calleeName, ctx.request.symbolTable).found)
    {
        return ExpressionType{calleeName, true, false, false};
    }
    if (IsBuiltInValueType(calleeName, ctx))
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

ExpressionType ResolveLiteralValueType(TSNode node, const DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symNumberLiteral)
    {
        return ExpressionType{ClassifyNumberLiteral(parser::NodeText(node, ctx.request.sourceCode)), true, true};
    }
    if (sym == gs.symStringLiteral)
    {
        return ExpressionType{std::string(ctx.request.GetEffectiveStringTypeName()), true, true};
    }
    if (sym == gs.symBooleanLiteral)
    {
        return ExpressionType{"bool", true, true};
    }
    return ExpressionType{};
}

ExpressionType ResolveUnaryValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(node, ctx.request.nodeIndex);
    const std::string op =
        parser::NodeText(syntax.Field(parser::GetGrammarSymbols().fieldOperator), ctx.request.sourceCode);
    if (op == "!" || op == "not")
    {
        return ExpressionType{"bool", true, false};
    }
    auto val =
        TypeConversionEvaluator::ResolveValueType(syntax.Field(parser::GetGrammarSymbols().fieldOperand), scope, ctx);
    if (op == "-")
    {
        if (val.baseName == "uint" || val.baseName == "uint32")
        {
            val.baseName = "int";
        }
        else if (val.baseName == "uint8")
        {
            val.baseName = "int8";
        }
        else if (val.baseName == "uint16")
        {
            val.baseName = "int16";
        }
        else if (val.baseName == "uint64")
        {
            val.baseName = "int64";
        }
    }
    return val;
}

ExpressionType ResolveCastValueType(TSNode node, const DiagnosticContext& ctx)
{
    const std::string rawText =
        parser::NodeText(SyntaxOperands(node, ctx.request.nodeIndex).Field(parser::GetGrammarSymbols().fieldType),
                         ctx.request.sourceCode);
    const auto qualifiers = ExtractTypeQualifiersView(rawText);
    const std::string typeText = CleanBaseType(rawText);
    return typeText.empty() ? ExpressionType{} : ExpressionType{typeText, true, false, qualifiers.isConst};
}

ExpressionType ResolveCompoundValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    node = UnwrapIndexedParentheses(node, ctx.request.nodeIndex);
    if (ts_node_is_null(node))
    {
        return ExpressionType{};
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symUnaryExpression)
    {
        return ResolveUnaryValueType(node, scope, ctx);
    }

    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return ResolveIdentifierValueType(node, scope, ctx);
    }

    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression)
    {
        return ResolveCallValueType(node, ctx);
    }

    if (sym == gs.symCastExpression || sym == gs.symFunctionalCastExpression)
    {
        return ResolveCastValueType(node, ctx);
    }

    if (sym == gs.symMemberExpression)
    {
        const std::string resolved = ResolveExpressionType(node, ExpressionTypeContext(scope, ctx));
        const auto qualifiers = ExtractTypeQualifiersView(resolved);
        return resolved.empty() ? ExpressionType{}
                                : ExpressionType{CleanBaseType(resolved), true, false, qualifiers.isConst};
    }

    return ExpressionType{};
}

ExpressionType TypeConversionEvaluator::ResolveValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    node = UnwrapIndexedParentheses(node, ctx.request.nodeIndex);
    if (ts_node_is_null(node))
    {
        return ExpressionType{};
    }
    if (ts_node_is_error(node) || ts_node_is_missing(node) ||
        (ctx.request.nodeIndex && ctx.request.nodeIndex->NodeHasSyntaxError(node)))
    {
        return ExpressionType{std::string(ExpressionTypeCache::k_poisonType), false, false};
    }

    const auto& gs = parser::GetGrammarSymbols();

    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symNumberLiteral || sym == gs.symStringLiteral || sym == gs.symBooleanLiteral)
    {
        return ResolveLiteralValueType(node, ctx);
    }
    if (sym == gs.symNullLiteral)
    {
        return ExpressionType{};
    }

    return ResolveCompoundValueType(node, scope, ctx);
}

ExpressionType ResolveValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    return TypeConversionEvaluator::ResolveValueType(node, scope, ctx);
}

} // namespace angel_lsp::analysis
