#pragma once

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace lsp {
    struct Position {
        uint32_t line = 0;
        uint32_t character = 0;
    };

    struct Range {
        Position start;
        Position end;
    };

    struct TextEdit {
        Range range;
        std::string newText;
    };
}

class Document {
public:
    Document(std::string uri, std::string text);
    ~Document();

    // Prevent copies as we manage TSTree* and TSParser*
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;

    // Incremental update (from didChange LSP event)
    void ApplyEdit(const lsp::TextEdit& edit);

    // Tree access
    TSNode RootNode() const;
    TSNode NodeAt(uint32_t line, uint32_t col) const;

    // Helpers
    std::string_view SourceAt(TSNode node) const;
    const std::string& GetUri() const { return uri; }
    const std::string& GetText() const { return text; }

private:
    std::string uri;
    std::string text;

    TSParser* parser;
    TSTree* tree;

    size_t ToByteOffset(uint32_t line, uint32_t col) const;
    TSPoint ByteToPoint(size_t byteOffset) const;
};
