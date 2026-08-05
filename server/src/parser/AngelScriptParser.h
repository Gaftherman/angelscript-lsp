#pragma once

#include "utils/LspLogger.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{
    class AngelScriptParser
    {
    private:
        TSParser *m_parser;
        utils::LspLogger *m_logger;

    public:
        AngelScriptParser(utils::LspLogger *logger = nullptr);
        ~AngelScriptParser();
        TSTree *Parse(const std::string &sourceCode, TSTree *oldTree = nullptr);
        static std::string_view GetNodeText(TSNode node, const std::string &sourceCode);
        TSParser *GetRawParser() const { return m_parser; }
    };
}