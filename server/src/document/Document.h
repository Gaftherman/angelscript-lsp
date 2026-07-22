/**
 * @file Document.h
 * @brief Thread-safe document container for source code text and Tree-Sitter AST parsing.
 * @ingroup Core
 */

#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <tree_sitter/api.h>

#include <lsp/types.h>

namespace angel_lsp
{
    namespace document
    {
        /**
         * @brief Custom RAII deleter for Tree-Sitter TSParser pointers.
         */
        struct TSParserDeleter
        {
            void operator()(TSParser *parser) const
            {
                if (parser)
                {
                    ts_parser_delete(parser);
                }
            }
        };

        /**
         * @brief Custom RAII deleter for Tree-Sitter TSTree pointers.
         */
        struct TSTreeDeleter
        {
            void operator()(TSTree *tree) const
            {
                if (tree)
                {
                    ts_tree_delete(tree);
                }
            }
        };

        using UniqueTSParser = std::unique_ptr<TSParser, TSParserDeleter>;
        using UniqueTSTree = std::unique_ptr<TSTree, TSTreeDeleter>;
    } // namespace document

    /**
     * @brief Represents a text document managed by the LSP, including source code and Tree-Sitter AST.
     * @note Thread-safe for concurrent read-only operations after initialization.
     */
    class Document
    {
    public:
        /**
         * @brief Constructs a new Document.
         *
         * @param[in] uri The URI of the document.
         * @param[in] text The initial source code text.
         */
        Document(std::string uri, std::string text);

        /**
         * @brief Destroys the Document and cleans up Tree-Sitter resources via RAII.
         */
        ~Document();

        // Prevent copies as we manage TSTree* and TSParser*
        Document(const Document &) = delete;
        Document &operator=(const Document &) = delete;
        Document(Document &&) noexcept;
        Document &operator=(Document &&) noexcept;

        /**
         * @brief Applies an incremental text edit to the document and updates the AST.
         *
         * @param[in] edit The LSP text edit to apply.
         */
        void ApplyEdit(const lsp::TextEdit &edit);

        /**
         * @brief Gets the root node of the Tree-Sitter AST.
         *
         * @return TSNode The root node of the parsed syntax tree.
         * @note Returns a null node if parsing failed or tree is uninitialized.
         */
        TSNode RootNode() const;

        /**
         * @brief Gets the AST node at the specified line and column.
         *
         * @param[in] line Zero-indexed line number.
         * @param[in] col Zero-indexed column number.
         * @return TSNode The innermost node containing the position.
         * @note Uses Tree-Sitter byte offset calculation.
         */
        TSNode NodeAt(uint32_t line, uint32_t col) const;

        /**
         * @brief Extracts the source code substring corresponding to a given AST node.
         *
         * @param[in] node The AST node to extract text for.
         * @return std::string_view A view into the source text.
         */
        std::string_view SourceAt(TSNode node) const;

        /**
         * @brief Gets the document URI.
         * @return const std::string& The document URI string.
         */
        const std::string &GetUri() const { return uri; }

        /**
         * @brief Gets the full document text.
         * @return const std::string& The full source code string.
         */
        const std::string &GetText() const { return text; }

    private:
        std::string uri;
        std::string text;

        document::UniqueTSParser parser;
        document::UniqueTSTree tree;

        size_t ToByteOffset(uint32_t line, uint32_t col) const;
        TSPoint ByteToPoint(size_t byteOffset) const;
    };

    namespace document
    {
        using ::angel_lsp::Document;
    }
} // namespace angel_lsp

using Document = angel_lsp::Document;
