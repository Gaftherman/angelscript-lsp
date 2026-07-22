#pragma once

#include <string>
#include <memory>
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolResolver.h"

class asIScriptEngine;

namespace angel_lsp::test
{

    /**
     * @brief Helper utility to create an in-memory Document for testing.
     * @param uri Document URI (e.g., "file:///test.as").
     * @param content AngelScript source code content.
     * @return Initialized Document object.
     */
    inline Document CreateTestDocument(const std::string &uri, const std::string &content)
    {
        return Document(uri, content);
    }

    /**
     * @brief Helper utility to populate a SymbolTable from an in-memory document.
     * @param doc The in-memory Document instance.
     * @param table The target SymbolTable instance to populate.
     * @param asEngine Optional AngelScript engine pointer for built-in types.
     */
    inline void PopulateTestSymbolTable(const Document &doc, analysis::SymbolTable &table)
    {
        analysis::SymbolCollector::CollectGlobals(doc, table);
        analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);
    }

} // namespace angel_lsp::test
