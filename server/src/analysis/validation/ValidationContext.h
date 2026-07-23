/**
 * @file ValidationContext.h
 * @brief Context structure passed to validation rules in the pipeline.
 * @ingroup Analysis
 */

#pragma once

#include <tree_sitter/api.h>
#include <functional>
#include <string>
#include <vector>
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolResolver.h"
#include "i18n/LspStrings.h"

namespace analysis::validation
{
    /**
     * @brief Immutable context containing document, symbol tables, resolver, and Tree-Sitter AST root.
     */
    struct ValidationContext
    {
        const Document &document;
        const SymbolTable &symbolTable;
        const SymbolResolver &symbolResolver;
        TSNode rootNode;
        const SymbolTable *globalTable = nullptr;
        i18n::Locale locale = i18n::Locale::EN;
        std::function<const Document *(const std::string &)> docResolver = nullptr;
    };
}
