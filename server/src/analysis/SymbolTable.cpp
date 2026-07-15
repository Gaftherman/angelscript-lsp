#include "analysis/SymbolTable.h"

namespace analysis
{
    SymbolTable::SymbolTable() = default;
    
    SymbolTable::~SymbolTable() = default;

    void SymbolTable::AddGlobal(std::shared_ptr<Symbol> symbol)
    {
        if (!symbol) return;
        m_globalSymbols[symbol->name] = std::move(symbol);
    }

    void SymbolTable::AddLocal(std::shared_ptr<Symbol> symbol)
    {
        if (!symbol) return;
        m_localSymbols.push_back(std::move(symbol));
    }

    Symbol* SymbolTable::FindGlobalByName(const std::string& name) const
    {
        auto it = m_globalSymbols.find(name);
        if (it != m_globalSymbols.end())
        {
            return it->second.get();
        }
        return nullptr;
    }

    Symbol* SymbolTable::FindLocalByName(const std::string& name) const
    {
        // Search backwards to return the most recently declared local (shadowing)
        for (auto it = m_localSymbols.rbegin(); it != m_localSymbols.rend(); ++it)
        {
            if ((*it)->name == name)
                return it->get();
        }
        return nullptr;
    }

    Symbol* SymbolTable::FindScopeByPosition(uint32_t line, uint32_t col) const
    {
        // Currently we don't have line/col in Symbol struct directly (we have fullRange which is an lsp::Range).
        // A simple linear scan through locals, then globals, checking if the position falls inside fullRange.
        
        auto isInside = [](const lsp::Range& r, uint32_t l, uint32_t c) {
            if (l < r.start.line || l > r.end.line) return false;
            if (l == r.start.line && c < r.start.character) return false;
            if (l == r.end.line && c > r.end.character) return false;
            return true;
        };

        // Check locals first (more specific scopes usually added later or nested)
        for (auto it = m_localSymbols.rbegin(); it != m_localSymbols.rend(); ++it)
        {
            if (isInside((*it)->fullRange, line, col))
            {
                return it->get();
            }
        }

        // Then globals
        for (const auto& [name, sym] : m_globalSymbols)
        {
            if (isInside(sym->fullRange, line, col))
            {
                return sym.get();
            }
        }

        return nullptr;
    }

    void SymbolTable::ClearLocals()
    {
        m_localSymbols.clear();
    }
    
    void SymbolTable::ClearAll()
    {
        m_localSymbols.clear();
        m_globalSymbols.clear();
    }
}
