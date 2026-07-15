#pragma once

#include "document/Document.h"
#include "analysis/SymbolTable.h"

namespace analysis
{
    class SymbolResolver
    {
    public:
        // Returns the symbol corresponding to the identifier under the cursor, or nullptr if none found.
        static const Symbol* ResolveAt(const Document& doc, const SymbolTable& table, uint32_t line, uint32_t character);
        
    private:
        static std::string CleanTypeName(std::string_view raw);
    };
}
