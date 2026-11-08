#pragma once

#include "parser/SyntaxTree.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::document
{

/**
 * @brief Snapshot parameters for constructing a Document.
 */
struct DocumentSnapshot
{
    /**
     * @brief Canonical document URI.
     */
    std::string uri;

    /**
     * @brief Document text content.
     */
    std::string text;

    /**
     * @brief Document version number.
     */
    int version = 0;

    /**
     * @brief Monotonic document generation counter.
     */
    uint64_t generation = 0;

    /**
     * @brief Transformed or sanitized source text when preprocessing was applied.
     */
    std::shared_ptr<const std::string> transformedSource = {};
};

/**
 * @brief In-memory document managed by the language server.
 */
struct Document
{
    /**
     * @brief Canonical document URI.
     */
    std::string uri;

    /**
     * @brief Document text content.
     */
    std::string text;

    /**
     * @brief Document version number.
     */
    int version = 0;

    /**
     * @brief Monotonic document generation counter.
     */
    uint64_t generation = 0;

    /**
     * @brief Managed syntax tree for the document.
     */
    parser::TreePtr tree = parser::MakeTreePtr(nullptr);

    /**
     * @brief Constructs an empty Document.
     */
    Document() = default;

    /**
     * @brief Constructs a Document from uri, text, version, and syntax tree.
     * @param[in] u Document URI.
     * @param[in] t Document text.
     * @param[in] v Document version.
     * @param[in] tr Parsed syntax tree handle.
     */
    Document(std::string u, std::string t, int v, parser::TreePtr tr)
        : uri(std::move(u)), text(std::move(t)), version(v), generation(0), tree(std::move(tr))
    {
    }

    /**
     * @brief Constructs a Document from a snapshot and syntax tree.
     * @param[in] snapshot Document snapshot parameters.
     * @param[in] tr Parsed syntax tree handle.
     */
    Document(DocumentSnapshot snapshot, parser::TreePtr tr)
        : uri(std::move(snapshot.uri)), text(std::move(snapshot.text)), version(snapshot.version),
          generation(snapshot.generation), tree(std::move(tr)),
          transformedSource_(std::move(snapshot.transformedSource))
    {
    }

    /**
     * @brief Copy constructor duplicating the syntax tree.
     * @param[in] other Document instance to copy from.
     */
    Document(const Document& other)
        : uri(other.uri), text(other.text), version(other.version), generation(other.generation),
          tree(parser::CopyTree(other.tree.get())), transformedSource_(other.transformedSource_)
    {
    }

    /**
     * @brief Move constructor transferring document ownership.
     * @param[in,out] other Document instance to move from; left with an empty tree.
     */
    Document(Document&& other) noexcept = default;

    /**
     * @brief Copy assignment operator duplicating the syntax tree.
     * @param[in] other Document instance to copy from.
     * @return Reference to this Document.
     */
    Document& operator=(const Document& other)
    {
        if (this != &other)
        {
            uri = other.uri;
            text = other.text;
            version = other.version;
            generation = other.generation;
            tree = parser::CopyTree(other.tree.get());
            transformedSource_ = other.transformedSource_;
        }
        return *this;
    }

    /**
     * @brief Move assignment operator transferring document ownership.
     * @param[in,out] other Document instance to move from; left with an empty tree.
     * @return Reference to this Document.
     */
    Document& operator=(Document&& other) noexcept = default;

    /**
     * @brief Returns the exact bytes parsed into the document tree.
     * @return String view of the parsed source text.
     */
    [[nodiscard]] std::string_view ParsedSource() const noexcept
    {
        return transformedSource_ ? std::string_view(*transformedSource_) : std::string_view(text);
    }

    /**
     * @brief Returns the shared pointer to transformed source text if available.
     * @return Shared pointer to const string containing transformed source.
     */
    [[nodiscard]] const std::shared_ptr<const std::string>& TransformedSource() const noexcept
    {
        return transformedSource_;
    }

  private:
    /**
     * @brief Transformed or sanitized source text when preprocessing is active.
     */
    std::shared_ptr<const std::string> transformedSource_;
};

} // namespace angel_lsp::document
