#pragma once

#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <optional>
#include <string_view>

namespace angel_lsp::features
{
    /**
     * @brief Context and options for document formatting.
     */
    struct FormattingRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::FormattingOptions options;
    };

    /**
     * @brief Context and options for range formatting.
     */
    struct RangeFormattingRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::Range range;
        lsp::FormattingOptions options;
    };

    using FormattingResult = std::vector<lsp::TextEdit>;

    // Type aliases for naming compatibility
    using DocumentFormattingRequest = FormattingRequest;
    using DocumentRangeFormattingRequest = RangeFormattingRequest;

    /**
     * @brief Formats an entire AngelScript document according to options and Allman style.
     * @param request Immutable formatting request context.
     * @return List of TextEdits (or std::nullopt if formatting failed / no edits needed).
     */
    std::optional<std::vector<lsp::TextEdit>> FormatDocument(const FormattingRequest &request);

    /**
     * @brief Formats a specific range in an AngelScript document.
     * @param request Immutable range formatting request context.
     * @return List of TextEdits for the specified range.
     */
    std::optional<std::vector<lsp::TextEdit>> FormatRange(const RangeFormattingRequest &request);

    /**
     * @brief Directly formats an AngelScript source code string.
     * @param sourceCode Source text to format.
     * @param options Formatting configuration options.
     * @return Formatted source code string.
     */
    std::string FormatSourceCode(std::string_view sourceCode, const lsp::FormattingOptions &options);
}
