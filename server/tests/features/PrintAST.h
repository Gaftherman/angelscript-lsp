#include <iostream>
#include <fstream>
#include "document/Document.h"
#include "analysis/SymbolCollector.h"

void PrintASTNode(TSNode node, const Document& doc, int depth)
{
    std::string indent(depth * 2, ' ');
    std::string type = ts_node_type(node);
    std::string text = analysis::SymbolCollector::GetNodeText(node, doc);
    
    // Replace newlines in text with space to keep it single line
    for (char& c : text) if (c == '\n' || c == '\r') c = ' ';
    if (text.length() > 50) text = text.substr(0, 47) + "...";

    std::cout << indent << "[" << type << "] '" << text << "'\n";

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_child(node, i);
        PrintASTNode(child, doc, depth + 1);
    }
}
