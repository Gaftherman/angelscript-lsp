#include "parser/DoxygenParser.h"

#include <cstdint>
#include <string_view>
#include <utility>

extern "C" const TSLanguage* tree_sitter_doxygen(void);

namespace angel_lsp::parser
{
DoxygenParser::DoxygenParser() : m_parser(ts_parser_new())
{
    ts_parser_set_language(m_parser, tree_sitter_doxygen());
}

DoxygenParser::~DoxygenParser()
{
    if (m_parser)
    {
        ts_parser_delete(m_parser);
        m_parser = nullptr;
    }
}

DoxygenParser::DoxygenParser(DoxygenParser&& other) noexcept : m_parser(other.m_parser)
{
    other.m_parser = nullptr;
}

DoxygenParser& DoxygenParser::operator=(DoxygenParser&& other) noexcept
{
    if (this != &other)
    {
        if (m_parser)
        {
            ts_parser_delete(m_parser);
        }
        m_parser = other.m_parser;
        other.m_parser = nullptr;
    }
    return *this;
}

TSTree* DoxygenParser::Parse(std::string_view text)
{
    // A moved-from parser owns nothing, and ts_parser_parse_string does not check - it would be
    // a crash rather than a diagnostic, which is the wrong way round for a doc comment.
    if (m_parser == nullptr || text.empty())
    {
        return nullptr;
    }

    return ts_parser_parse_string(m_parser, nullptr, text.data(), static_cast<uint32_t>(text.size()));
}

} // namespace angel_lsp::parser
