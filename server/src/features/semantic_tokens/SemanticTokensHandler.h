/**
 * @file SemanticTokensHandler.h
 * @brief Handler for LSP textDocument/semanticTokens/full syntax highlighting requests.
 * @ingroup Features
 */

#pragma once

#include <lsp/messages.h>
#include "document/Document.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolResolver.h"
#include <vector>
#include <cstdint>
#include <string>

namespace angel_lsp::features::semantic_tokens
{
    /**
     * @brief LSP Semantic Token types according to standard LSP specification.
     */
    enum class TokenType : uint32_t
    {
        Namespace = 0,
        Type = 1,
        Class = 2,
        Enum = 3,
        Interface = 4,
        Struct = 5,
        TypeParameter = 6,
        Parameter = 7,
        Variable = 8,
        Property = 9,
        EnumMember = 10,
        Event = 11,
        Function = 12,
        Method = 13,
        Macro = 14,
        Keyword = 15,
        Modifier = 16,
        Comment = 17,
        String = 18,
        Number = 19,
        Regexp = 20,
        Operator = 21
    };

    /**
     * @brief LSP Semantic Token modifiers bitmask flags.
     */
    enum class TokenModifier : uint32_t
    {
        None = 0,
        Declaration = 1 << 0,
        ReadOnly = 1 << 1,
        Static = 1 << 2,
        Deprecated = 1 << 3,
        Documentation = 1 << 4
    };

    /**
     * @brief A single extracted absolute semantic token position before delta encoding.
     */
    struct SemanticToken
    {
        uint32_t line;
        uint32_t startChar;
        uint32_t length;
        uint32_t tokenType;
        uint32_t tokenModifiers;
    };

    /**
     * @brief Handler class for LSP textDocument/semanticTokens/full requests.
     * @note Thread-safe stateless semantic tokens provider class.
     */
    class SemanticTokensHandler
    {
    public:
        /**
         * @brief Handles LSP textDocument/semanticTokens/full request and returns encoded integer array.
         *
         * @param[in] doc The parsed Document containing Tree-Sitter AST and source code.
         * @param[in] table The SymbolTable containing resolved symbols.
         * @return std::vector<uint32_t> Delta-encoded 5-tuple integer array.
         */
        static std::vector<uint32_t> ProvideSemanticTokens(
            const Document &doc,
            const analysis::SymbolTable &table);

        /**
         * @brief Returns the supported token types legend array for LSP initialization.
         * @return std::vector<std::string> Vector of token type strings.
         */
        static std::vector<std::string> GetTokenTypesLegend();

        /**
         * @brief Returns the supported token modifiers legend array for LSP initialization.
         * @return std::vector<std::string> Vector of token modifier strings.
         */
        static std::vector<std::string> GetTokenModifiersLegend();
    };

    /**
     * @brief High level entry point for LSP textDocument/semanticTokens/full requests.
     *
     * @param[in] params Request parameters object.
     * @param[in] doc The parsed Document.
     * @param[in] table The SymbolTable.
     * @return lsp::requests::TextDocument_SemanticTokens_Full::Result Standard LSP semantic tokens result.
     * @note Thread-safe stateless provider function.
     */
    lsp::requests::TextDocument_SemanticTokens_Full::Result ProcessSemanticTokensFull(
        const lsp::requests::TextDocument_SemanticTokens_Full::Params &params,
        const Document &doc,
        const analysis::SymbolTable &table);
} // namespace angel_lsp::features::semantic_tokens

namespace angel_lsp::features
{
    using semantic_tokens::ProcessSemanticTokensFull;
    using semantic_tokens::SemanticToken;
    using semantic_tokens::SemanticTokensHandler;
    using semantic_tokens::TokenModifier;
    using semantic_tokens::TokenType;
}
