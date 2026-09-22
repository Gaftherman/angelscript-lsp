#pragma once

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

// Forward-declared so the tree-sitter parser abstraction can compile without the LSP protocol library.
namespace angel_lsp::utils
{
class LspLogger;
}

namespace angel_lsp::parser
{
class AngelScriptParser
{
  private:
    TSParser* m_parser;
    utils::LspLogger* m_logger;

  public:
    AngelScriptParser(utils::LspLogger* logger = nullptr);
    ~AngelScriptParser();
    /**
     * @brief Parses AngelScript source text into a Tree-Sitter AST tree.
     * @param[in] sourceCode Source text to parse.
     * @param[in] oldTree Optional previous AST for incremental re-parsing.
     * @return Pointer to parsed TSTree or nullptr on failure.
     */
    TSTree* Parse(std::string_view sourceCode, TSTree* oldTree = nullptr);

    /**
     * @brief Extracts the substring corresponding to an AST node.
     * @param[in] node AST node.
     * @param[in] sourceCode Source text.
     * @return String view slice of the node text.
     */
    static std::string_view GetNodeText(TSNode node, std::string_view sourceCode);
    TSParser* GetRawParser() const
    {
        return m_parser;
    }
};
} // namespace angel_lsp::parser