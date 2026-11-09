#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolTable.h"

#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Reads type information from direct operands with an iterative worklist.
 * @param[in] typeNode Type expression or primitive type node.
 * @param[in] sourceCode Source text for the same syntax revision.
 * @param[in] index Captured operands when available.
 * @return Type metadata with the base names of its immediate template arguments.
 */
TypeExtractionResult ExtractTypeInfoFromAST(TSNode typeNode, std::string_view sourceCode,
                                            const NodeIndex* index = nullptr);

/**
 * @brief True if valueNode's own top-level value is a null literal, e.g. "null" or "(null)".
 *        Unlike a full-tree scan, this does NOT recurse into call arguments, binary operands,
 */
bool IsNullInitializer(TSNode valueNode, const NodeIndex* index = nullptr);

/**
 * @brief Maps pre-resolved primitive token symbols to semantic type kinds.
 */
struct TypeExtractionSymbols
{
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

/// Worklist operations for reading type operands.
enum class TypeReadAction : uint8_t
{
    Evaluate,
    Child,
    Datatype,
    TemplateArgument,
    Finish
};

/// Pending work on one type or its direct child.
struct TypeReadStep
{
    TypeReadAction action;
    TSNode node{};
    TSNode previous{};
};

/// Metadata accumulated for an active type.
struct TypeReadFrame
{
    TypeExtractionResult result;
    std::string datatypeText;
    std::string scopePrefix;
};

} // namespace angel_lsp::analysis
