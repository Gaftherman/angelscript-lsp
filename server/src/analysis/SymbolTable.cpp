#include "analysis/SymbolTable.h"
#include <functional>

namespace analysis
{
    SymbolTable::SymbolTable() = default;

    SymbolTable::~SymbolTable() = default;

    void SymbolTable::AddGlobal(std::shared_ptr<Symbol> symbol)
    {
        if (!symbol)
            return;
        for (const auto &existing : m_globalSymbols[symbol->name])
        {
            if (existing.get() == symbol.get() ||
                (existing->name == symbol->name &&
                 existing->uri == symbol->uri &&
                 existing->selectionRange.start.line == symbol->selectionRange.start.line &&
                 existing->selectionRange.start.character == symbol->selectionRange.start.character))
            {
                return;
            }
        }
        m_globalSymbols[symbol->name].push_back(std::move(symbol));
    }

    void SymbolTable::AddLocal(std::shared_ptr<Symbol> symbol)
    {
        if (!symbol)
            return;
        m_localSymbols.push_back(std::move(symbol));
    }

    void SymbolTable::MergeGlobals(const SymbolTable &other)
    {
        for (const auto &[name, symbols] : other.GetGlobals())
        {
            for (const auto &sym : symbols)
            {
                m_globalSymbols[name].push_back(sym);
            }
        }
    }

    Symbol *SymbolTable::FindGlobalByName(const std::string &name) const
    {
        auto it = m_globalSymbols.find(name);
        if (it != m_globalSymbols.end() && !it->second.empty())
        {
            return it->second.front().get();
        }
        return nullptr;
    }

    std::vector<Symbol *> SymbolTable::FindAllGlobalsByName(const std::string &name) const
    {
        std::vector<Symbol *> results;
        auto it = m_globalSymbols.find(name);
        if (it != m_globalSymbols.end())
        {
            for (const auto &sym : it->second)
            {
                results.push_back(sym.get());
            }
        }
        return results;
    }

    const Symbol *SymbolTable::FindByNameDeep(const std::string &name) const
    {
        // 1. Search in globals first
        if (Symbol *global = FindGlobalByName(name))
        {
            return global;
        }

        // Handle scoped names like "Engine::Physics" or "Engine::Physics::BodyPriority"
        if (name.find("::") != std::string::npos)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            size_t end = name.find("::");
            while (end != std::string::npos)
            {
                parts.push_back(name.substr(start, end - start));
                start = end + 2;
                end = name.find("::", start);
            }
            parts.push_back(name.substr(start));

            const Symbol *current = nullptr;
            for (const std::string &part : parts)
            {
                if (!current)
                {
                    current = FindGlobalByName(part);
                }
                else
                {
                    const Symbol *next = nullptr;
                    for (const auto &child : current->children)
                    {
                        if (child->name == part)
                        {
                            next = child.get();
                            break;
                        }
                        if (child->kind == SymbolKind::Enum)
                        {
                            for (const auto &eMem : child->children)
                            {
                                if (eMem->name == part)
                                {
                                    next = eMem.get();
                                    break;
                                }
                            }
                            if (next) break;
                        }
                    }
                    current = next;
                }
                if (!current) break;
            }
            if (current) return current;
        }

        // 2. Deep search inside namespaces
        for (const auto &[nsName, nsSyms] : m_globalSymbols)
        {
            for (const auto &nsSym : nsSyms)
            {
                if (nsSym->kind == SymbolKind::Namespace)
                {
                    auto searchChildren = [&](auto &self, const Symbol *currentNs) -> const Symbol *
                    {
                        // Check direct children and enum members
                        for (const auto &child : currentNs->children)
                        {
                            if (child->name == name)
                            {
                                return child.get();
                            }
                            if (child->kind == SymbolKind::Enum)
                            {
                                for (const auto &eMem : child->children)
                                {
                                    if (eMem->name == name)
                                    {
                                        return eMem.get();
                                    }
                                }
                            }
                        }
                        // Recurse into nested namespaces
                        for (const auto &child : currentNs->children)
                        {
                            if (child->kind == SymbolKind::Namespace)
                            {
                                if (const Symbol *found = self(self, child.get()))
                                {
                                    return found;
                                }
                            }
                        }
                        return nullptr;
                    };

                    if (const Symbol *found = searchChildren(searchChildren, nsSym.get()))
                    {
                        return found;
                    }
                }
            }
        }
        return nullptr;
    }

    std::vector<const Symbol *> SymbolTable::FindHostClassesOf(const std::string &mixinName) const
    {
        std::vector<const Symbol *> result;
        for (const auto &[name, syms] : m_globalSymbols)
        {
            for (const auto &sym : syms)
            {
                if (sym->kind != SymbolKind::Class)
                    continue;
                for (const auto &base : sym->baseClasses)
                {
                    if (base == mixinName)
                    {
                        result.push_back(sym.get());
                        break;
                    }
                }
            }
        }
        return result;
    }

    Symbol *SymbolTable::FindLocalByName(const std::string &name) const
    {
        // Search backwards to return the most recently declared local (shadowing)
        for (auto it = m_localSymbols.rbegin(); it != m_localSymbols.rend(); ++it)
        {
            if ((*it)->name == name)
                return it->get();
        }
        return nullptr;
    }

    const Symbol *SymbolTable::FindLocalByNameAt(const std::string &name, uint32_t line, uint32_t col) const
    {
        auto isInside = [](const lsp::Range &r, uint32_t l, uint32_t c)
        {
            if (l < r.start.line || l > r.end.line)
                return false;
            if (l == r.start.line && c < r.start.character)
                return false;
            if (l == r.end.line && c > r.end.character)
                return false;
            return true;
        };

        const Symbol *bestMatch = nullptr;
        uint32_t bestScore = 0xFFFFFFFF; // Smaller score = tighter scope

        for (const auto &sym : m_localSymbols)
        {
            if (sym->name == name && isInside(sym->fullRange, line, col))
            {
                uint32_t lineSpan = sym->fullRange.end.line - sym->fullRange.start.line;
                // Add column difference to break ties
                uint32_t colSpan = 0;
                if (lineSpan == 0)
                {
                    colSpan = sym->fullRange.end.character - sym->fullRange.start.character;
                }

                uint32_t score = (lineSpan << 16) | (colSpan & 0xFFFF);
                if (score <= bestScore)
                {
                    bestScore = score;
                    bestMatch = sym.get();
                }
            }
        }
        return bestMatch;
    }

    std::vector<const Symbol *> SymbolTable::FindByName(const std::string &name) const
    {
        std::vector<const Symbol *> results;
        for (const auto &sym : m_localSymbols)
        {
            if (sym->name == name)
            {
                results.push_back(sym.get());
            }
        }

        auto it = m_globalSymbols.find(name);
        if (it != m_globalSymbols.end())
        {
            for (const auto &sym : it->second)
            {
                results.push_back(sym.get());
            }
        }
        return results;
    }

    Symbol *SymbolTable::FindFirst(const std::string &name) const
    {
        Symbol *local = FindLocalByName(name);
        if (local)
            return local;
        return FindGlobalByName(name);
    }

    std::vector<const Symbol *> SymbolTable::FindInContainer(const std::string &containerName) const
    {
        std::vector<const Symbol *> result;
        Symbol *container = FindGlobalByName(containerName);
        if (container)
        {
            for (const auto &child : container->children)
            {
                result.push_back(child.get());
            }
        }
        return result;
    }

    Symbol *SymbolTable::FindScopeByPosition(const std::string &uri, uint32_t line, uint32_t col) const
    {
        auto isInside = [&uri](const Symbol *s, uint32_t l, uint32_t c)
        {
            if (!s->uri.empty() && s->uri != uri)
                return false;
            const lsp::Range &r = s->fullRange;
            if (l < r.start.line || l > r.end.line)
                return false;
            if (l == r.start.line && c < r.start.character)
                return false;
            if (l == r.end.line && c > r.end.character)
                return false;
            return true;
        };

        // Check locals first
        for (auto it = m_localSymbols.rbegin(); it != m_localSymbols.rend(); ++it)
        {
            if (isInside(it->get(), line, col))
            {
                return it->get();
            }
        }

        // Helper to find deepest match
        std::function<Symbol *(Symbol *)> findDeepest = [&](Symbol *current) -> Symbol *
        {
            for (const auto &child : current->children)
            {
                if (isInside(child.get(), line, col))
                {
                    Symbol *deeper = findDeepest(child.get());
                    return deeper ? deeper : child.get();
                }
            }
            return nullptr;
        };

        for (const auto &[name, syms] : m_globalSymbols)
        {
            for (const auto &sym : syms)
            {
                if (isInside(sym.get(), line, col))
                {
                    Symbol *deeper = findDeepest(sym.get());
                    return deeper ? deeper : sym.get();
                }
            }
        }

        return nullptr;
    }

    void SymbolTable::ClearLocals()
    {
        m_localSymbols.clear();
    }

    void SymbolTable::AddUsingNamespace(const std::string &ns)
    {
        m_usingNamespaces.push_back(ns);
    }

    const std::vector<std::string> &SymbolTable::GetUsingNamespaces() const
    {
        return m_usingNamespaces;
    }

    void SymbolTable::ClearAll()
    {
        m_localSymbols.clear();
        m_globalSymbols.clear();
    }
}
