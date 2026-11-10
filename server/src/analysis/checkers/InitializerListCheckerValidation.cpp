#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <string>
#include <string_view>
#include <variant>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a type is a universal variant proxy class with opAssign(?&in) and zero fields.
 * @param[in] type Cleaned type name string view.
 * @param[in] valCtx Active list validation context.
 * @return True if the type behaves as a universal proxy class, false otherwise.
 */
static bool IsUniversalProxyClass(std::string_view type, const ListValidationContext& valCtx)
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

/**
 * @brief Checks if an initializer list is being assigned to a subscript on an associative container.
 * @param[in] list Initializer list fact.
 * @param[in] type Cleaned target type string view.
 * @param[in] valCtx Active list validation context.
 * @return True if the assignment target is an associative container subscript, false otherwise.
 */
static bool IsAssociativeSubscriptTarget(const InitializerListFact& list, std::string_view type,
                                         const ListValidationContext& valCtx)
{
    if (list.owner.kind != InitializerListOwnerKind::Assignment || !list.owner.isSubscriptAssignment)
    {
        return false;
    }
    if (!list.owner.subscriptObjectBytes.Empty())
    {
        const std::string objType = ResolveExpressionType(list.owner.subscriptObjectBytes,
                                                          ExpressionTypeContext(valCtx.elements.scopeRoot, valCtx.ctx));
        if (!objType.empty() && InspectDictionary(ReadTemplateSpelling(objType), valCtx.ctx).isDict)
        {
            return true;
        }
    }
    if (list.owner.hasStringIndex)
    {
        const AggregateStructInfo structInfo =
            InspectAggregateStruct(ReadTemplateSpelling(type), valCtx.ctx, valCtx.cache);
        return structInfo.fields.empty() && !structInfo.hasListSupport;
    }
    return false;
}

/**
 * @brief Reports whether an initializer list target type cannot accept an initializer list.
 * @param[in] list Initializer list fact.
 * @param[in] type Cleaned type name string view.
 * @param[in] valCtx Active list validation context.
 * @return True if the target type does not support initializer lists.
 */
static bool IsUnsupportedTargetType(const InitializerListFact& list, std::string_view type,
                                    const ListValidationContext& valCtx)
{
    return IsCorePrimitive(type) || type == "?" || IsUniversalProxyClass(type, valCtx) ||
           IsAssociativeSubscriptTarget(list, type, valCtx);
}

/**
 * @brief Emits a hint diagnostic if an uninstantiated type symbol is encountered with no list support.
 * @param[in] list Initializer list fact.
 * @param[in] type Cleaned type name string view.
 * @param[in] valCtx Active list validation context.
 */
static void CheckUnknownListPattern(const InitializerListFact& list, std::string_view type,
                                    const ListValidationContext& valCtx)
{
    if (valCtx.depth == 0 && valCtx.ctx.request.symbolTable.HasSymbolAnywhere(type) &&
        !valCtx.ctx.request.IsRegisteredSymbol(type))
    {
        valCtx.ctx.EmitAtRange(list.range, "as-hint-list-pattern-unknown", type, DiagnosticSeverity::Hint);
    }
}

void InitializerListValidator::ValidateList(const InitializerListFact& list, std::string_view targetType,
                                            const ListValidationContext& valCtx)
{
    const std::string type = StripDecorations(targetType);
    if (type.empty() || type == "auto" || type == "void")
    {
        return;
    }

    if (IsUnsupportedTargetType(list, type, valCtx))
    {
        valCtx.ctx.EmitAtRange(list.range, "as-err-initializer-list-not-supported", type);
        return;
    }

    const TemplateSpelling spelling = ReadTemplateSpelling(type);

    // Dictionary types
    const DictInfo dictInfo = InspectDictionary(spelling, valCtx.ctx);
    if (dictInfo.isDict)
    {
        ValidateDictionaryList(list, dictInfo, valCtx);
        return;
    }

    // Sequence & multi-dimensional container types
    const ListContainerInfo containerInfo = InspectContainer(type, spelling, valCtx.ctx);
    if (containerInfo.isContainer)
    {
        ValidateMultiDimensionalContainer(list, containerInfo.dimensions, containerInfo.elementType, valCtx);
        return;
    }

    // Class / Struct Aggregates
    const AggregateStructInfo structInfo = InspectAggregateStruct(spelling, valCtx.ctx, valCtx.cache);
    if (!structInfo.fields.empty())
    {
        ValidateStructAggregateList(list, type, structInfo, valCtx);
        return;
    }

    CheckUnknownListPattern(list, type, valCtx);
}

} // namespace angel_lsp::analysis
