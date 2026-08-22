#pragma once

#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::document
{
    /**
     * @brief Represents an in-memory document managed by the language server.
     */
    struct Document
    {
        std::string uri;
        std::string text;
        int version = 0;
        TSTree *tree = nullptr;
    };
}
