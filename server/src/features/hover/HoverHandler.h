#pragma once

#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
#include "analysis/ScopeTree.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <functional>
#include <string>
#include <optional>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a hover request.
     */
    struct HoverRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
        lsp::Position position;

        /**
         * @brief Reads a document's text by URI, or returns nullptr when the server holds none.
         *
         * A documentation comment lives above the *declaration*, which is often in another file,
         * and `sourceCode` is the file being hovered over. Reading one at the other's line numbers
         * produced whatever happened to be on that line here - so hovering a symbol declared at
         * line 12 of a header could show a comment from line 12 of the current file, about
         * something else entirely. Wrong documentation is worse than none, which is what an absent
         * reader gets: no comment rather than a guess.
         *
         * The same shape as CompletionResolveRequest::readDocument, and supplied from the same
         * place in Server.cpp.
         */
        std::function<const std::string *(const std::string &)> readDocument;

        /**
         * @brief The server's configuration, for the engine properties that change what is legal.
         *
         * asEP_PROPERTY_ACCESSOR_MODE above all: whether `get_X` is the property `X` is the host's
         * setting, not a fact about the source, and a hover that decided it locally would disagree
         * with the diagnostics on the same line.
         */
        const config::ServerConfig *config = nullptr;

        /**
         * @brief Resolves the text inside an `#include` to an absolute path, or "" when it misses.
         *
         * Supplied by the caller rather than done here, for the reason readDocument is: resolution
         * needs the configured search directories and the allow-list that confines them, and both
         * live on the server. A null callback means the include line is answered with the raw text
         * and nothing more, which is what a unit test with no filesystem should see.
         *
         * Hovering `#include "helper.as"` is the one place a user asks "which file is that,
         * exactly" - a relative path resolves against the current file, then against each search
         * directory in order, and the answer is not guessable from the line itself.
         */
        std::function<std::string(const std::string &rawPath)> resolveInclude;
    };

    /**
     * @brief Computes hover tooltip information for symbol under cursor.
     * @param request Immutable context for hover computation.
     * @return Optional Hover object, nullopt if no symbol found.
     */
    std::optional<lsp::Hover> GetHover(const HoverRequest &request);

}
