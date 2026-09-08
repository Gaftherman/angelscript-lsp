#include "parser/DoxygenParser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

extern "C" const TSLanguage *tree_sitter_doxygen(void);

namespace angel_lsp::parser
{
    DoxygenParser::DoxygenParser()
        : m_parser(ts_parser_new())
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

    DoxygenParser::DoxygenParser(DoxygenParser &&other) noexcept
        : m_parser(other.m_parser)
    {
        other.m_parser = nullptr;
    }

    DoxygenParser &DoxygenParser::operator=(DoxygenParser &&other) noexcept
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

    TSTree *DoxygenParser::Parse(const std::string &text)
    {
        // A moved-from parser owns nothing, and ts_parser_parse_string does not check - it would be
        // a crash rather than a diagnostic, which is the wrong way round for a doc comment.
        if (m_parser == nullptr || text.empty())
        {
            return nullptr;
        }

        return ts_parser_parse_string(m_parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    }

    std::string_view DoxygenParser::GetNodeText(TSNode node, const std::string &sourceCode)
    {
        if (ts_node_is_null(node))
        {
            return std::string_view();
        }

        uint32_t startByte = ts_node_start_byte(node);
        uint32_t endByte = ts_node_end_byte(node);

        if (startByte >= endByte || endByte > sourceCode.size())
        {
            return std::string_view();
        }

        return std::string_view(sourceCode.data() + startByte, endByte - startByte);
    }
}
