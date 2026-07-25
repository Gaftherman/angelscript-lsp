#include "AngelScriptParser.h"
#include "utils/LspLogger.h"
#include "spdlog/fmt/fmt.h"

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::parser
{
    AngelScriptParser::AngelScriptParser(angel_lsp::utils::LspLogger *logger)
    {
        m_logger = logger;
        m_parser = ts_parser_new();
        ts_parser_set_language(m_parser, tree_sitter_angelscript());
    }

    AngelScriptParser::~AngelScriptParser()
    {
        ts_parser_delete(m_parser);
    }

    void AngelScriptParser::PrintNode(TSNode node, int depth, const std::string &sourceCode)
    {
        if (!m_logger)
            return;

        std::string indent(depth * 2, ' ');
        const char *nodeType = ts_node_type(node);
        uint32_t startByte = ts_node_start_byte(node);
        uint32_t endByte = ts_node_end_byte(node);
        uint32_t childCount = ts_node_child_count(node);

        // If leaf node (no children), print node type and its text value cleanly
        if (childCount == 0 && startByte < endByte && endByte <= sourceCode.size())
        {
            std::string textSlice = sourceCode.substr(startByte, endByte - startByte);
            m_logger->LogInfo(fmt::format("{}- {} -> \"{}\"", indent, nodeType, textSlice));
        }
        else
        {
            m_logger->LogInfo(fmt::format("{}- {}", indent, nodeType));
        }

        for (uint32_t i = 0; i < childCount; ++i)
        {
            TSNode childNode = ts_node_child(node, i);
            PrintNode(childNode, depth + 1, sourceCode);
        }
    }

    void AngelScriptParser::Parse(const std::string &sourceCode)
    {
        if (sourceCode.empty())
            return;

        TSTree *tree = ts_parser_parse_string(m_parser, nullptr, sourceCode.c_str(), static_cast<uint32_t>(sourceCode.size()));

        if (!tree)
        {
            if (m_logger)
                m_logger->LogError("Failed to parse source code with Tree-sitter.");
            return;
        }

        TSNode rootNode = ts_tree_root_node(tree);
        PrintNode(rootNode, 0, sourceCode);

        ts_tree_delete(tree);
    }
}