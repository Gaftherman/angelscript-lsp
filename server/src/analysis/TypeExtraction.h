#pragma once

#include "analysis/SymbolTable.h"
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
TypeExtractionResult ExtractTypeInfoFromAST(TSNode typeNode, std::string_view sourceCode);

/**
 * @brief True if valueNode's own top-level value is a null literal, e.g. "null" or "(null)".
 *        Unlike a full-tree scan, this does NOT recurse into call arguments, binary operands,
 *        or index expressions, so "someFunc(null)" or "a == null" correctly return false.
 */
bool IsNullInitializer(TSNode valueNode);

/**
 * @brief Pre-resolved grammar symbols and sentinel IDs used during fast AST type extraction.
 */
struct TypeExtractionSymbols
{
    TSSymbol symPrimitiveType = 0;
    TSSymbol symDatatype = 0;
    TSSymbol symTemplateTypeList = 0;
    TSSymbol symIdentifier = 0;
    TSSymbol symType = 0;
    TSSymbol symScope = 0;
    TSSymbol symNullLiteral = 0;
    TSSymbol symParenthesizedExpression = 0;

    ankerl::unordered_dense::map<TSSymbol, TypeKind> primitiveKindMap;

    TypeExtractionSymbols();

    TypeKind LookupPrimitiveKind(TSSymbol symbol) const
    {
        auto it = primitiveKindMap.find(symbol);
        if (it != primitiveKindMap.end())
        {
            return it->second;
        }
        return TypeKind::Unknown;
    }
};
} // namespace angel_lsp::analysis
