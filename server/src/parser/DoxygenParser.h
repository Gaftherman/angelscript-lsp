#pragma once

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{
    /**
     * @brief RAII owner of a tree-sitter parser configured for the Doxygen grammar.
     *
     * Manages the lifecycle of a TSParser instance initialized with tree_sitter_doxygen.
     * Provides an interface for parsing Doxygen comments into tree-sitter syntax trees.
     */
    class DoxygenParser
    {
    private:
        TSParser *m_parser;

    public:
        /** @brief Allocates a new tree-sitter parser and binds the Doxygen language grammar. */
        DoxygenParser();

        /** @brief Releases the underlying tree-sitter parser resource. */
        ~DoxygenParser();

        DoxygenParser(const DoxygenParser &) = delete;
        DoxygenParser &operator=(const DoxygenParser &) = delete;

        DoxygenParser(DoxygenParser &&other) noexcept;
        DoxygenParser &operator=(DoxygenParser &&other) noexcept;

        /**
         * @brief Parses the given Doxygen comment string.
         *
         * @param text The comment string to parse.
         * @return TSTree pointer representing the parse tree, or nullptr on empty input.
         *         Caller takes ownership and must release it with ts_tree_delete.
         */
        TSTree *Parse(const std::string &text);

        /**
         * @brief Extracts a string_view of the source slice covered by a syntax node.
         *
         * @param node The syntax tree node whose byte range is queried.
         * @param sourceCode The original source buffer that was parsed.
         * @return View over the node's byte range, or an empty view if invalid or out of range.
         */
        static std::string_view GetNodeText(TSNode node, const std::string &sourceCode);

        /** @brief Returns the raw TSParser pointer. */
        TSParser *GetRawParser() const { return m_parser; }
    };
}
