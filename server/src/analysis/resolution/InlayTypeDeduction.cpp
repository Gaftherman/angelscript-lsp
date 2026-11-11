#include "analysis/resolution/InlayTypeDeduction.h"

#include "analysis/resolution/ExpressionTypeResolver.h"
#include "parser/Keywords.h"

#include <cstdint>
#include <string>

namespace angel_lsp::analysis
{

std::string DeduceExpressionTypeAt(uint32_t startByte, uint32_t endByte, const ExpressionTypeContext& ctx)
{
    if (startByte == 0 && endByte == 0)
    {
        return "";
    }
    std::string resolved = ResolveExpressionTypeAt(IndexedByteRange{startByte, endByte}, ctx);
    if (!resolved.empty() && resolved != parser::keywords::Auto)
    {
        return resolved;
    }
    return "";
}

} // namespace angel_lsp::analysis
