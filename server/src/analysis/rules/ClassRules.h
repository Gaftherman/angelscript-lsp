#pragma once

#include "analysis/DiagnosticContext.h"
#include <ankerl/unordered_dense.h>
#include <string>

namespace angel_lsp::analysis::rules
{
    /**
     * @brief Validates all semantic rules for a class symbol.
     * @param sym Class symbol to validate.
     * @param ctx Diagnostic context for error reporting.
     */
    void ValidateClass(const Symbol &sym, const DiagnosticContext &ctx);

    /**
     * @brief Checks if a class has circular inheritance in the symbol table.
     * @param currentClass Class name to check.
     * @param table Symbol table.
     * @param visited Set of visited class names.
     * @return True if circular inheritance detected.
     */
    bool CheckCircularInheritance(const std::string &currentClass, const SymbolTable &table, ankerl::unordered_dense::set<std::string> &visited);
}
