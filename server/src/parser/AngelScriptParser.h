#pragma once

#include "parser/SyntaxTree.h"
#include "utils/LspLogger.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{
class AngelScriptParser
{
  private:
    TSParser* m_parser;
    utils::LspLogger* m_logger;

  public:
    explicit AngelScriptParser(utils::LspLogger* logger = nullptr);
    ~AngelScriptParser();
    /**
     * @brief Parses AngelScript source text into a Tree-Sitter AST tree.
     * @param[in] sourceCode Source text to parse.
     * @param[in] oldTree Optional previous AST for incremental re-parsing.
     * @return Pointer to parsed TSTree or nullptr on failure.
     */
    TSTree* Parse(std::string_view sourceCode, TSTree* oldTree = nullptr);

    /**
     * @brief Parses AngelScript source text into a managed TreePtr.
     * @param[in] sourceCode Source text to parse.
     * @param[in] oldTree Optional previous AST for incremental re-parsing.
     * @return Managed TreePtr or null on failure.
     */
    TreePtr ParseTree(std::string_view sourceCode, const TreePtr& oldTree = nullptr)
    {
        return MakeTreePtr(Parse(sourceCode, oldTree ? oldTree.get() : nullptr));
    }

    TSParser* GetRawParser() const
    {
        return m_parser;
    }
};
} // namespace angel_lsp::parser