#pragma once

#include "analysis/SymbolTable.h"
#include <string>
#include <vector>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    TypeExtractionResult ExtractTypeInfoFromAST(TSNode typeNode, const std::string &sourceCode);
}
