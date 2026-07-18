#pragma once

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

#include <lsp/types.h>

/**
 * @brief Represents a text document managed by the LSP, including its source code and Tree-Sitter AST.
 */
class Document
{
public:
    /**
     * @brief Constructs a new Document.
     * 
     * @param uri The URI of the document.
     * @param text The initial source code text.
     */
    Document(std::string uri, std::string text);
    
    /**
     * @brief Destroys the Document and cleans up Tree-Sitter resources.
     */
    ~Document();

    // Prevent copies as we manage TSTree* and TSParser*
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;

    /**
     * @brief Applies an incremental text edit to the document and updates the AST.
     * 
     * @param edit The LSP text edit to apply.
     */
    void ApplyEdit(const lsp::TextEdit& edit);

    /**
     * @brief Gets the root node of the Tree-Sitter AST.
     * 
     * @return TSNode The root node.
     */
    TSNode RootNode() const;

    /**
     * @brief Gets the AST node at the specified line and column.
     * 
     * @param line Zero-indexed line number.
     * @param col Zero-indexed column number.
     * @return TSNode The innermost node containing the position.
     */
    TSNode NodeAt(uint32_t line, uint32_t col) const;

    /**
     * @brief Extracts the source code substring corresponding to a given AST node.
     * 
     * @param node The AST node.
     * @return std::string_view A view into the source text.
     */
    std::string_view SourceAt(TSNode node) const;

    /**
     * @brief Gets the document URI.
     */
    const std::string& GetUri() const { return uri; }

    /**
     * @brief Gets the full document text.
     */
    const std::string& GetText() const { return text; }

private:
    std::string uri;
    std::string text;

    TSParser* parser;
    TSTree* tree;

    size_t ToByteOffset(uint32_t line, uint32_t col) const;
    TSPoint ByteToPoint(size_t byteOffset) const;
};
