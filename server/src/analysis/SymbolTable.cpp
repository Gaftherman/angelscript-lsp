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

    const Symbol* SymbolTable::FindByNameDeep(const std::string& name) const
    {
        // 1. Search in globals first
        if (Symbol* global = FindGlobalByName(name))
        {
            return global;
        }

        // 2. Deep search inside namespaces
        for (const auto& [nsName, nsSym] : m_globalSymbols)
        {
            if (nsSym->kind == SymbolKind::Namespace)
            {
                // Recursive lambda to search inside a namespace
                auto searchChildren = [&](auto& self, const Symbol* currentNs) -> const Symbol* {
                    // Check direct children
                    for (const auto& child : currentNs->children)
                    {
                        if (child->name == name)
                        {
                            return child.get();
                        }
                    }
                    // Recurse into nested namespaces
                    for (const auto& child : currentNs->children)
                    {
                        if (child->kind == SymbolKind::Namespace)
                        {
                            if (const Symbol* found = self(self, child.get()))
                            {
                                return found;
                            }
                        }
                    }
                    return nullptr;
                };

                if (const Symbol* found = searchChildren(searchChildren, nsSym.get()))
                {
                    return found;
                }
            }
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

    std::vector<const Symbol*> SymbolTable::FindByName(const std::string& name) const
    {
        std::vector<const Symbol*> results;
        for (const auto& sym : m_localSymbols) {
            if (sym->name == name) results.push_back(sym.get());
        }
        auto it = m_globalSymbols.find(name);
        if (it != m_globalSymbols.end()) {
            results.push_back(it->second.get());
        }
        return results;
    }

    Symbol* SymbolTable::FindFirst(const std::string& name) const
    {
        Symbol* local = FindLocalByName(name);
        if (local) return local;
        return FindGlobalByName(name);
    }

    std::vector<const Symbol*> SymbolTable::FindInContainer(const std::string& containerName) const
    {
        std::vector<const Symbol*> result;
        Symbol* container = FindGlobalByName(containerName);
        if (container) {
            for (const auto& child : container->children) {
                result.push_back(child.get());
            }
        }
        return result;
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

    void SymbolTable::AddUsingNamespace(const std::string& ns)
    {
        m_usingNamespaces.push_back(ns);
    }

    const std::vector<std::string>& SymbolTable::GetUsingNamespaces() const
    {
        return m_usingNamespaces;
    }
    
    void SymbolTable::ClearAll()
    {
        m_localSymbols.clear();
        m_globalSymbols.clear();
    }
}
