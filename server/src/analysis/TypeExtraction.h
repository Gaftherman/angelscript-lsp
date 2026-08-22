#pragma once

#include "analysis/SymbolTable.h"
#include <string>
#include <vector>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    TypeExtractionResult ExtractTypeInfoFromAST(TSNode typeNode, const std::string &sourceCode);

    /**
     * @brief True if valueNode's own top-level value is a null literal, e.g. "null" or "(null)".
     *        Unlike a full-tree scan, this does NOT recurse into call arguments, binary operands,
     *        or index expressions, so "someFunc(null)" or "a == null" correctly return false.
     */
    bool IsNullInitializer(TSNode valueNode);
}
