/**
 * @file Document.cpp
 * @brief Implementation of Document class and Tree-Sitter AST parser wrapper.
 * @ingroup Core
 */

#include "Document.h"
#include <utility>
#include <stdexcept>

extern "C" TSLanguage *tree_sitter_angelscript();

namespace angel_lsp
{
    Document::Document(std::string uri, std::string text)
        : uri(std::move(uri)), text(std::move(text)), parser(ts_parser_new()), tree(nullptr)
    {
        if (parser)
        {
            ts_parser_set_language(parser.get(), tree_sitter_angelscript());
            tree.reset(ts_parser_parse_string(
                parser.get(), nullptr,
                this->text.c_str(), (uint32_t)this->text.size()));
        }
    }

    Document::~Document() = default;

    Document::Document(Document &&other) noexcept = default;
    Document &Document::operator=(Document &&other) noexcept = default;

    size_t Document::ToByteOffset(uint32_t line, uint32_t col) const
    {
        size_t offset = 0;
        uint32_t currentLine = 0;
        uint32_t currentCol = 0;

        while (offset < text.size())
        {
            if (currentLine == line && currentCol == col)
            {
                return offset;
            }

            if (text[offset] == '\n')
            {
                currentLine++;
                currentCol = 0;
            }
            else
            {
                currentCol++;
            }
            offset++;
        }

        return text.size(); // If position is out of bounds, clamp to end
    }

    TSPoint Document::ByteToPoint(size_t byteOffset) const
    {
        TSPoint point = {0, 0};
        for (size_t i = 0; i < byteOffset && i < text.size(); ++i)
        {
            if (text[i] == '\n')
            {
                point.row++;
                point.column = 0;
            }
            else
            {
                point.column++;
            }
        }
        return point;
    }

    void Document::ApplyEdit(const lsp::TextEdit &edit)
    {
        size_t startByte = ToByteOffset(edit.range.start.line, edit.range.start.character);
        size_t oldEndByte = ToByteOffset(edit.range.end.line, edit.range.end.character);

        TSPoint startPoint = ByteToPoint(startByte);
        TSPoint oldEndPoint = ByteToPoint(oldEndByte);

        // Apply the string replacement
        text.replace(startByte, oldEndByte - startByte, edit.newText);

        size_t newEndByte = startByte + edit.newText.size();
        TSPoint newEndPoint = ByteToPoint(newEndByte);

        if (!tree || !parser)
        {
            return;
        }

        TSInputEdit tsEdit = {
            .start_byte = (uint32_t)startByte,
            .old_end_byte = (uint32_t)oldEndByte,
            .new_end_byte = (uint32_t)newEndByte,
            .start_point = startPoint,
            .old_end_point = oldEndPoint,
            .new_end_point = newEndPoint,
        };

        ts_tree_edit(tree.get(), &tsEdit);

        tree.reset(ts_parser_parse_string(
            parser.get(), tree.get(),
            text.c_str(), (uint32_t)text.size()));
    }

    TSNode Document::RootNode() const
    {
        if (!tree)
        {
            return {{0, 0, 0, 0}, nullptr, nullptr};
        }
        return ts_tree_root_node(tree.get());
    }

    TSNode Document::NodeAt(uint32_t line, uint32_t col) const
    {
        if (!tree)
        {
            return {{0, 0, 0, 0}, nullptr, nullptr};
        }
        TSPoint point = {line, col};
        return ts_node_descendant_for_point_range(ts_tree_root_node(tree.get()), point, point);
    }

    std::string_view Document::SourceAt(TSNode node) const
    {
        if (ts_node_is_null(node))
        {
            return "";
        }
        size_t start = ts_node_start_byte(node);
        size_t end = ts_node_end_byte(node);
        if (end > text.size())
        {
            end = text.size();
        }
        if (start >= end)
        {
            return "";
        }
        return std::string_view(text.c_str() + start, end - start);
    }
} // namespace angel_lsp
