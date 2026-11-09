#include "AngelScriptParser.h"

#include "utils/LspLogger.h"

#include <spdlog/fmt/fmt.h>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::parser
{
AngelScriptParser::AngelScriptParser(utils::LspLogger* logger) : m_logger(logger)
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

TSTree* AngelScriptParser::Parse(std::string_view sourceCode, TSTree* oldTree)
{
    if (sourceCode.empty())
    {
        return ts_parser_parse_string(m_parser, oldTree, "", 0);
    }

    TSTree* tree =
        ts_parser_parse_string(m_parser, oldTree, sourceCode.data(), static_cast<uint32_t>(sourceCode.size()));

    if (!tree && m_logger)
    {
        m_logger->LogError("Failed to parse source code with Tree-sitter.");
    }

    return tree;
}

} // namespace angel_lsp::parser