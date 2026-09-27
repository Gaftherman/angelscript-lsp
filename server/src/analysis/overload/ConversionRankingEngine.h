#pragma once

#include "analysis/OverloadResolver.h"
#include "analysis/SymbolTable.h"

#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Context passed during argument-to-parameter type matching.
 */
struct MatchContext
{
    std::string cleanArg;
    std::string cleanParam;
    bool argIsHandle = false;
    bool argIsConst = false;
    bool paramIsHandle = false;
    bool paramIsConst = false;
    bool isMutableRef = false;
    const SymbolTable& table;
};
} // namespace angel_lsp::analysis
