#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"

#include <string>
#include <variant>

namespace angel_lsp::analysis
{
/**
 * @brief Checks if a type is a universal variant proxy class with opAssign(?&in) and zero fields.
 */
static bool IsUniversalProxyClass(const std::string& type, const ListValidationContext& valCtx)
{
    const auto opAssignSyms = valCtx.ctx.request.symbolTable.FindMemberSymbolPtr(type, "opAssign");
    if (!opAssignSyms)
    {
        return false;
    }
    for (const auto& sym : *opAssignSyms)
    {
        if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }
        const auto& fn = sym.GetFunction();
        if (!fn.parameters.empty() && fn.parameters[0].baseTypeName == "?")
        {
            const AggregateStructInfo structInfo =
                InspectAggregateStruct(ReadTemplateSpelling(type), valCtx.ctx, valCtx.cache);
            return structInfo.fields.empty() && !structInfo.hasListSupport;
        }
    }
    return false;
}

static bool HasStringIndex(TSNode left)
{
    TSNode idx = parser::GetChildByField(left, parser::GetGrammarSymbols().fieldIndex);
    if (ts_node_is_null(idx))
    {
        uint32_t namedCount = 0;
        parser::ForEachChildNode(left,
                                 [&](TSNode child)
                                 {
                                     if (ts_node_is_named(child))
                                     {
                                         if (namedCount == 1)
                                         {
                                             idx = child;
                                         }
                                         ++namedCount;
                                     }
                                 });
    }
    if (!ts_node_is_null(idx))
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(idx);
        return sym == gs.symStringLiteral || sym == gs.symConcatenatedString;
    }
    return false;
}

/**
 * @brief Checks if an initializer list is being assigned to a subscript on an associative container.
 */
static bool IsAssociativeSubscriptTarget(TSNode listNode, const std::string& type, const ListValidationContext& valCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode parent = parser::GetParent(listNode);
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symAssignmentExpression)
    {
        return false;
    }
    TSNode left = parser::GetChildByField(parent, gs.fieldLeft);
    if (ts_node_is_null(left) || ts_node_symbol(left) != gs.symIndexExpression)
    {
        return false;
    }
    TSNode obj = parser::GetChildByField(left, gs.fieldObject);
    if (ts_node_is_null(obj))
    {
        parser::ForEachChildNode(left,
                                 [&](TSNode child)
                                 {
                                     if (ts_node_is_null(obj) && ts_node_is_named(child))
                                     {
                                         obj = child;
                                     }
                                 });
    }
    if (ts_node_is_null(obj))
    {
        return false;
    }
    const std::string objType =
        ResolveExpressionType(obj, ExpressionTypeContext(valCtx.elements.scopeRoot, valCtx.ctx));
    if (objType.empty())
    {
        return false;
    }
    if (InspectDictionary(ReadTemplateSpelling(objType), valCtx.ctx).isDict)
    {
        return true;
    }
    if (HasStringIndex(left))
    {
        const AggregateStructInfo structInfo =
            InspectAggregateStruct(ReadTemplateSpelling(type), valCtx.ctx, valCtx.cache);
        return structInfo.fields.empty() && !structInfo.hasListSupport;
    }
    return false;
}

static bool IsUnsupportedTargetType(TSNode listNode, const std::string& type, const ListValidationContext& valCtx)
{
    return IsCorePrimitive(type) || type == "?" || IsUniversalProxyClass(type, valCtx) ||
           IsAssociativeSubscriptTarget(listNode, type, valCtx);
}

static void CheckUnknownListPattern(TSNode listNode, const std::string& type, const ListValidationContext& valCtx)
{
    if (valCtx.depth == 0 && valCtx.ctx.request.symbolTable.HasSymbolAnywhere(type) &&
        !valCtx.ctx.request.IsRegisteredSymbol(type))
    {
        const TSPoint start = ts_node_start_point(listNode);
        const TSPoint end = ts_node_end_point(listNode);
        valCtx.ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-hint-list-pattern-unknown", type,
                               DiagnosticSeverity::Hint);
    }
}

void InitializerListValidator::ValidateList(TSNode listNode, const std::string& targetType,
                                            const ListValidationContext& valCtx)
{
    if (ts_node_is_null(listNode))
    {
        return;
    }

    const std::string type = StripDecorations(targetType);
    if (type.empty() || type == "auto" || type == "void")
    {
        return;
    }

    if (IsUnsupportedTargetType(listNode, type, valCtx))
    {
        valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(listNode), "as-err-initializer-list-not-supported", type);
        return;
    }

    const TemplateSpelling spelling = ReadTemplateSpelling(type);

    // Dictionary types
    const DictInfo dictInfo = InspectDictionary(spelling, valCtx.ctx);
    if (dictInfo.isDict)
    {
        ValidateDictionaryList(listNode, dictInfo, valCtx);
        return;
    }

    // Sequence & multi-dimensional container types
    const ListContainerInfo containerInfo = InspectContainer(type, spelling, valCtx.ctx);
    if (containerInfo.isContainer)
    {
        ValidateMultiDimensionalContainer(listNode, containerInfo.dimensions, containerInfo.elementType, valCtx);
        return;
    }

    // Class / Struct Aggregates
    const AggregateStructInfo structInfo = InspectAggregateStruct(spelling, valCtx.ctx, valCtx.cache);
    if (!structInfo.fields.empty())
    {
        ValidateStructAggregateList(listNode, type, structInfo, valCtx);
        return;
    }

    CheckUnknownListPattern(listNode, type, valCtx);
}
} // namespace angel_lsp::analysis
