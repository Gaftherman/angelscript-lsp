#pragma once
#include <ankerl/unordered_dense.h>
#include <string>
#include <vector>
#include <memory>
#include "Symbol.h"

namespace analysis
{
    class SymbolTable
    {
    public:
        SymbolTable();
        ~SymbolTable();

        void AddGlobal(std::shared_ptr<Symbol> symbol);
        void AddLocal(std::shared_ptr<Symbol> symbol);

        // Find a symbol whose definition range contains the given (line, col)
        // Usually for finding what scope we are currently inside.
        Symbol* FindScopeByPosition(uint32_t line, uint32_t col) const;

        // Find a symbol by exact name at global scope
        Symbol* FindGlobalByName(const std::string& name) const;
        
        // Find a symbol by exact name at local scope
        Symbol* FindLocalByName(const std::string& name) const;

        // Find all symbols with the exact name (e.g. overloads)
        std::vector<const Symbol*> FindByName(const std::string& name) const;

        // Find first symbol (local or global) by name
        Symbol* FindFirst(const std::string& name) const;

        // Find symbols inside a specific container (e.g. class or namespace)
        std::vector<const Symbol*> FindInContainer(const std::string& containerName) const;

        const ankerl::unordered_dense::map<std::string, std::shared_ptr<Symbol>>& GetGlobals() const { return m_globalSymbols; }
        const std::vector<std::shared_ptr<Symbol>>& GetLocals() const { return m_localSymbols; }

        // Clears only local symbols (e.g. before re-parsing a function body)
        void ClearLocals();
        
        // Clears everything
        void ClearAll();

    private:
        // unordered_dense is extremely fast for lookups and insertions
        ankerl::unordered_dense::map<std::string, std::shared_ptr<Symbol>> m_globalSymbols;
        
        // Locals are typically small in number and ordered by appearance
        std::vector<std::shared_ptr<Symbol>> m_localSymbols;
    };
}
