#include "AngelScriptParser.h"

#include <spdlog/fmt/fmt.h>

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::parser
{
    AngelScriptParser::AngelScriptParser(utils::LspLogger *logger)
        : m_logger(logger)
    {
        m_parser = ts_parser_new();
        ts_parser_set_language(m_parser, tree_sitter_angelscript());
    }

    AngelScriptParser::~AngelScriptParser()
    {
        if (m_parser)
        {
            ts_parser_delete(m_parser);
        }
    }

    TSTree *AngelScriptParser::Parse(const std::string &sourceCode, TSTree *oldTree)
    {
        if (sourceCode.empty())
        {
            if (m_logger)
                m_logger->LogError("Source code is empty.");
            return nullptr;
        }

        TSTree *tree = ts_parser_parse_string(m_parser, oldTree, sourceCode.c_str(), static_cast<uint32_t>(sourceCode.size()));

        if (!tree && m_logger)
        {
            m_logger->LogError("Failed to parse source code with Tree-sitter.");
        }

        return tree;
    }

    std::string_view AngelScriptParser::GetNodeText(TSNode node, const std::string &sourceCode)
    {
        if (ts_node_is_null(node))
            return std::string_view();

        uint32_t start_byte = ts_node_start_byte(node);
        uint32_t end_byte = ts_node_end_byte(node);

        if (start_byte >= end_byte || end_byte > sourceCode.size())
            return std::string_view();

        return std::string_view(sourceCode.data() + start_byte, end_byte - start_byte);
    }
}