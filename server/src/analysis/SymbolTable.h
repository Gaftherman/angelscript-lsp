/**
 * @file SymbolTable.h
 * @brief Container for global and local AngelScript symbols and scope resolution tables.
 * @ingroup Analysis
 */

#pragma once
#include <ankerl/unordered_dense.h>
#include <string>
#include <vector>
#include <memory>
#include "Symbol.h"

namespace analysis
{
    /**
     * @brief A table containing all globally and locally scoped symbols for a document context.
     * @note Thread-safe for concurrent read operations once populated.
     */
    class SymbolTable
    {
    public:
        SymbolTable();
        ~SymbolTable();

        /**
         * @brief Adds a globally scoped symbol.
         *
         * @param[in] symbol The symbol to add.
         */
        void AddGlobal(std::shared_ptr<Symbol> symbol);

        /**
         * @brief Adds a locally scoped symbol.
         *
         * @param[in] symbol The symbol to add.
         */
        void AddLocal(std::shared_ptr<Symbol> symbol);

        /**
         * @brief Merges global symbols from another SymbolTable into this one.
         *
         * @param[in] other The source table to copy global symbols from.
         */
        void MergeGlobals(const SymbolTable &other);

        /**
         * @brief Registers a using namespace directive for the file context.
         *
         * @param[in] ns The namespace name string.
         */
        void AddUsingNamespace(const std::string &ns);

        /**
         * @brief Retrieves all registered using namespaces.
         *
         * @return const std::vector<std::string>& A list of using namespace strings.
         */
        const std::vector<std::string> &GetUsingNamespaces() const;

        /**
         * @brief Finds a symbol whose definition range contains the cursor position.
         *
         * @param[in] uri The document URI.
         * @param[in] line Zero-indexed line number.
         * @param[in] col Zero-indexed column number.
         * @return Symbol* The containing scope symbol, or nullptr if none.
         */
        Symbol *FindScopeByPosition(const std::string &uri, uint32_t line, uint32_t col) const;

        /**
         * @brief Finds the first global symbol matching a specific name.
         *
         * @param[in] name The exact symbol name.
         * @return Symbol* The matched global symbol, or nullptr.
         */
        Symbol *FindGlobalByName(const std::string &name) const;

        /**
         * @brief Finds all global symbols matching a specific name (e.g. function overloads).
         *
         * @param[in] name The exact symbol name.
         * @return std::vector<Symbol*> A list of matched symbols.
         */
        std::vector<Symbol *> FindAllGlobalsByName(const std::string &name) const;

        /**
         * @brief Performs a deep recursive search for a symbol by name across all children.
         *
         * @param[in] name The exact symbol name.
         * @return const Symbol* The first matched symbol, or nullptr.
         */
        const Symbol *FindByNameDeep(const std::string &name) const;

        /**
         * @brief Finds all classes that inherit from or include a specified mixin.
         *
         * @param[in] mixinName The name of the target mixin.
         * @return std::vector<const Symbol*> A list of host class symbols.
         */
        std::vector<const Symbol *> FindHostClassesOf(const std::string &mixinName) const;

        /**
         * @brief Finds the first local symbol matching a specific name.
         *
         * @param[in] name The exact local variable name.
         * @return Symbol* The matched local symbol, or nullptr.
         */
        Symbol *FindLocalByName(const std::string &name) const;

        /**
         * @brief Finds a local symbol by name, ensuring it is visible at the given cursor position.
         *
         * @param[in] name The exact local variable name.
         * @param[in] line Zero-indexed line number.
         * @param[in] col Zero-indexed column number.
         * @return const Symbol* The valid local symbol, or nullptr.
         */
        const Symbol *FindLocalByNameAt(const std::string &name, uint32_t line, uint32_t col) const;

        /**
         * @brief Finds all symbols (global or local) matching a name.
         *
         * @param[in] name The symbol name string.
         * @return std::vector<const Symbol*> A list of matching symbols.
         */
        std::vector<const Symbol *> FindByName(const std::string &name) const;

        /**
         * @brief Finds the first symbol (local or global) matching a name.
         *
         * @param[in] name The symbol name string.
         * @return Symbol* The first matched symbol, or nullptr.
         */
        Symbol *FindFirst(const std::string &name) const;

        /**
         * @brief Finds all symbols directly inside a specified container (e.g., class or namespace).
         *
         * @param[in] containerName The container name string.
         * @return std::vector<const Symbol*> A list of child symbols.
         */
        std::vector<const Symbol *> FindInContainer(const std::string &containerName) const;

        /**
         * @brief Retrieves all global symbols map.
         * @return const ankerl::unordered_dense::map<std::string, std::vector<std::shared_ptr<Symbol>>>& Reference to the global symbols map.
         */
        const ankerl::unordered_dense::map<std::string, std::vector<std::shared_ptr<Symbol>>> &GetGlobals() const { return m_globalSymbols; }

        /**
         * @brief Retrieves all local symbols list.
         * @return const std::vector<std::shared_ptr<Symbol>>& Reference to the local symbols vector.
         */
        const std::vector<std::shared_ptr<Symbol>> &GetLocals() const { return m_localSymbols; }

        /**
         * @brief Clears all locally scoped symbols.
         */
        void ClearLocals();

        /**
         * @brief Clears all global and local symbols from the table.
         */
        void ClearAll();

    private:
        ankerl::unordered_dense::map<std::string, std::vector<std::shared_ptr<Symbol>>> m_globalSymbols;
        std::vector<std::shared_ptr<Symbol>> m_localSymbols;
        std::vector<std::string> m_usingNamespaces;
    };
}
