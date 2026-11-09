#include "analysis/resolution/OperandRangeResolution.h"

#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "analysis/resolution/ScopeSymbolLookup.h"

#include <optional>
#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves the type of the expression that spans the given byte range.
 * @param[in] bytes Byte span of the expression.
 * @param[in] ctx Expression context used for resolution.
 * @return Resolved type spelling, or an empty string when recovery fails.
 */
std::string ResolveExpressionType(IndexedByteRange bytes, const ExpressionTypeContext& ctx)
{
    const TSNode node = RecoverNodeAt(ctx, bytes.startByte, bytes.endByte);
    if (ts_node_is_null(node))
    {
        return {};
    }
    return ResolveExpressionType(node, ctx);
}

/**
 * @brief Reports the data type an expression names instead of a value.
 * @param[in] bytes Byte span of the expression.
 * @param[in] ctx Expression context used for resolution.
 * @return Data type name, or nullopt when the expression is a value or recovery fails.
 */
std::optional<std::string> IsBareDataType(IndexedByteRange bytes, const ExpressionTypeContext& ctx)
{
    const TSNode node = RecoverNodeAt(ctx, bytes.startByte, bytes.endByte);
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }
    return IsBareDataType(node, ctx);
}

/**
 * @brief Reports whether the expression that spans the given bytes names a function.
 * @param[in] bytes Byte span of the expression.
 * @param[in] ctx Expression context used for resolution.
 * @return True for a function reference, or false when not a function reference or recovery fails.
 */
bool IsFunctionReference(IndexedByteRange bytes, const ExpressionTypeContext& ctx)
{
    const TSNode node = RecoverNodeAt(ctx, bytes.startByte, bytes.endByte);
    if (ts_node_is_null(node))
    {
        return false;
    }
    return IsFunctionReference(node, ctx);
}

} // namespace angel_lsp::analysis
