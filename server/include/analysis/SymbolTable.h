#pragma once
#include <ankerl/unordered_dense.h>
#include <string>
#include <vector>
#include <memory>
#include "Symbol.h"

namespace analysis
{
    /**
     * @brief A table containing all globally and locally scoped symbols.
     * 
     * Manages efficient lookup, insertion, and scoping of AngelScript symbols
     * used heavily during LSP resolution tasks.
     */
    class SymbolTable
    {
    public:
        SymbolTable();
        ~SymbolTable();

        /**
         * @brief Adds a globally scoped symbol.
         * 
         * @param symbol The symbol to add.
         */
        void AddGlobal(std::shared_ptr<Symbol> symbol);

        /**
         * @brief Adds a locally scoped symbol.
         * 
         * @param symbol The symbol to add.
         */
        void AddLocal(std::shared_ptr<Symbol> symbol);
        
        /**
         * @brief Merges global symbols from another SymbolTable into this one.
         * 
         * @param other The table to merge globals from.
         */
        void MergeGlobals(const SymbolTable &other);

        /**
         * @brief Registers a `using namespace` directive for the current file context.
         * 
         * @param ns The namespace name.
         */
        void AddUsingNamespace(const std::string &ns);

        /**
         * @brief Retrieves all registered using namespaces.
         * 
         * @return A list of using namespaces.
         */
        const std::vector<std::string> &GetUsingNamespaces() const;

        /**
         * @brief Finds a symbol whose definition range contains the given cursor position.
         * 
         * @param line The 0-indexed line.
         * @param col The 0-indexed column.
         * @return The containing scope symbol, or nullptr if none.
         */
        Symbol *FindScopeByPosition(uint32_t line, uint32_t col) const;

        /**
         * @brief Finds the first global symbol matching a specific name.
         * 
         * @param name The exact symbol name.
         * @return The matched global symbol, or nullptr.
         */
        Symbol *FindGlobalByName(const std::string &name) const;

        /**
         * @brief Finds all global symbols matching a specific name (e.g. overloads).
         * 
         * @param name The exact symbol name.
         * @return A list of matched symbols.
         */
        std::vector<Symbol *> FindAllGlobalsByName(const std::string &name) const;

        /**
         * @brief Performs a deep recursive search for a symbol by name across all children.
         * 
         * @param name The exact symbol name.
         * @return The first matched symbol, or nullptr.
         */
        const Symbol *FindByNameDeep(const std::string &name) const;
        
        /**
         * @brief Finds all classes that inherit from or include the specified mixin.
         * 
         * @param mixinName The name of the mixin.
         * @return A list of host class symbols.
         */
        std::vector<const Symbol *> FindHostClassesOf(const std::string &mixinName) const;

        /**
         * @brief Finds the first local symbol matching a specific name.
         * 
         * @param name The exact local variable name.
         * @return The matched local symbol, or nullptr.
         */
        Symbol *FindLocalByName(const std::string &name) const;

        /**
         * @brief Finds a local symbol by name, ensuring it is visible at the given position.
         * 
         * @param name The exact local variable name.
         * @param line The 0-indexed line.
         * @param col The 0-indexed column.
         * @return The valid local symbol, or nullptr.
         */
        const Symbol *FindLocalByNameAt(const std::string &name, uint32_t line, uint32_t col) const;

        /**
         * @brief Finds all symbols (global or local) matching a name.
         * 
         * @param name The symbol name.
         * @return A list of matching symbols.
         */
        std::vector<const Symbol *> FindByName(const std::string &name) const;

        /**
         * @brief Finds the first symbol (local or global) matching a name.
         * 
         * @param name The symbol name.
         * @return The first matched symbol, or nullptr.
         */
        Symbol *FindFirst(const std::string &name) const;

        /**
         * @brief Finds all symbols directly inside a specified container (e.g., class or namespace).
         * 
         * @param containerName The container's name.
         * @return A list of child symbols.
         */
        std::vector<const Symbol *> FindInContainer(const std::string &containerName) const;

        /**
         * @brief Retrieves all global symbols.
         * 
         * @return The map of global symbols.
         */
        const ankerl::unordered_dense::map<std::string, std::vector<std::shared_ptr<Symbol>>> &GetGlobals() const { return m_globalSymbols; }

        /**
         * @brief Retrieves all local symbols.
         * 
         * @return The list of local symbols.
         */
        const std::vector<std::shared_ptr<Symbol>> &GetLocals() const { return m_localSymbols; }

        /**
         * @brief Clears all locally scoped symbols.
         */
        void ClearLocals();
        
        /**
         * @brief Clears all symbols from the table.
         */
        void ClearAll();

    private:
        ankerl::unordered_dense::map<std::string, std::vector<std::shared_ptr<Symbol>>> m_globalSymbols;
        std::vector<std::shared_ptr<Symbol>> m_localSymbols;
        std::vector<std::string> m_usingNamespaces;
    };
}
