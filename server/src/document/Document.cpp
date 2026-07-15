#include "Document.h"
#include <utility>
#include <stdexcept>

extern "C" TSLanguage* tree_sitter_angelscript();

Document::Document(std::string uri, std::string text)
    : uri(std::move(uri)), text(std::move(text)), tree(nullptr)
{
    parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_angelscript());
    
    tree = ts_parser_parse_string(
        parser, nullptr, 
        this->text.c_str(), (uint32_t)this->text.size()
    );
}

Document::~Document()
{
    if (tree) ts_tree_delete(tree);
    if (parser) ts_parser_delete(parser);
}

Document::Document(Document&& other) noexcept 
    : uri(std::move(other.uri)), text(std::move(other.text)), 
      parser(other.parser), tree(other.tree)
{
    other.parser = nullptr;
    other.tree = nullptr;
}

Document& Document::operator=(Document&& other) noexcept
{
    if (this != &other)
    {
        if (tree) ts_tree_delete(tree);
        if (parser) ts_parser_delete(parser);
        
        uri = std::move(other.uri);
        text = std::move(other.text);
        parser = other.parser;
        tree = other.tree;
        
        other.parser = nullptr;
        other.tree = nullptr;
    }
    return *this;
}

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
            // Note: simple character counting, ignores UTF-8 multi-byte logic for simplicity
            // A real LSP would use UTF-16 code unit offset or UTF-8 depending on client capabilities.
            currentCol++;
        }
        offset++;
    }
    
    return offset;
}

TSPoint Document::ByteToPoint(size_t byteOffset) const
{
    TSPoint point = {0, 0};
    size_t limit = std::min(byteOffset, text.size());
    
    for (size_t i = 0; i < limit; i++)
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

void Document::ApplyEdit(const lsp::TextEdit& edit)
{
    size_t startByte = ToByteOffset(edit.range.start.line, edit.range.start.character);
    size_t oldEndByte = ToByteOffset(edit.range.end.line, edit.range.end.character);
    size_t newEndByte = startByte + edit.newText.size();

    TSPoint startPoint = ByteToPoint(startByte);
    TSPoint oldEndPoint = ByteToPoint(oldEndByte);
    
    text.replace(startByte, oldEndByte - startByte, edit.newText);
    
    TSPoint newEndPoint = ByteToPoint(newEndByte);

    TSInputEdit tsEdit {
        .start_byte = (uint32_t)startByte,
        .old_end_byte = (uint32_t)oldEndByte,
        .new_end_byte = (uint32_t)newEndByte,
        .start_point = startPoint,
        .old_end_point = oldEndPoint,
        .new_end_point = newEndPoint,
    };
    
    ts_tree_edit(tree, &tsEdit);

    TSTree* newTree = ts_parser_parse_string(
        parser, tree,
        text.c_str(), (uint32_t)text.size()
    );
    
    ts_tree_delete(tree);
    tree = newTree;
}

TSNode Document::RootNode() const
{
    if (!tree) return {{0, 0, 0, 0}, nullptr, nullptr};
    return ts_tree_root_node(tree);
}

TSNode Document::NodeAt(uint32_t line, uint32_t col) const
{
    if (!tree) return {{0, 0, 0, 0}, nullptr, nullptr};
    TSPoint point = { line, col };
    return ts_node_descendant_for_point_range(ts_tree_root_node(tree), point, point);
}

std::string_view Document::SourceAt(TSNode node) const
{
    if (ts_node_is_null(node)) return "";
    size_t start = ts_node_start_byte(node);
    size_t end = ts_node_end_byte(node);
    if (end > text.size()) end = text.size();
    if (start >= end) return "";
    return std::string_view(text.c_str() + start, end - start);
}
