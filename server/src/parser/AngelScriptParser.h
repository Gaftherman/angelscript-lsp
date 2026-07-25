#pragma once

#include "utils/LspLogger.h"

#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{
    class AngelScriptParser
    {
    private:
        TSParser *m_parser;
        angel_lsp::utils::LspLogger *m_logger;

    public:
        AngelScriptParser(angel_lsp::utils::LspLogger *logger);
        ~AngelScriptParser();

        void Parse(const std::string &sourceCode);
        void PrintNode(TSNode node, int depth, const std::string &sourceCode);
    };
}