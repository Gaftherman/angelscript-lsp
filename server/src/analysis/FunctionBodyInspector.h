#pragma once

#include "analysis/SymbolTable.h"
#include <tree_sitter/api.h>
#include <string>

namespace angel_lsp::analysis
{
    /**
     * @brief Inspects a function body AST node to gather control flow, return statements, casts, and variables.
     * @param bodyNode The Tree-sitter AST node representing the function body statement block.
     * @param analysis The FunctionBodyAnalysis output where body inspection findings will be stored.
     * @param sourceCode Immutable reference to the full file source code.
     */
    void InspectFunctionBodyAST(TSNode bodyNode, FunctionBodyAnalysis &analysis, const std::string &sourceCode);
}
