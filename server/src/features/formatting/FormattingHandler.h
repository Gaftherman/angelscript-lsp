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
     * @brief Where a *block*'s opening brace goes.
     *
     * Only blocks. A brace that opens a value - an initializer list, a lambda body passed as an
     * argument - stays on its line under either style, because that is not a matter of taste:
     * `array<int> a =` followed by a lone `{` on the next line is what this formatter used to
     * produce, and it is wrong in every brace style there is.
     */
    enum class BraceStyle
    {
        Allman,  ///< `{` on its own line, aligned with the statement that owns it. The default.
        KAndR    ///< `{` at the end of the statement line, and `else` beside the `}` before it.
    };

    /**
     * @brief Context and options for document formatting.
     */
    struct FormattingRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::FormattingOptions options;
        BraceStyle braceStyle = BraceStyle::Allman;
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
        BraceStyle braceStyle = BraceStyle::Allman;
    };

    /**
     * @brief Context and options for on-type formatting.
     */
    struct OnTypeFormattingRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::Position position;
        std::string ch;
        lsp::FormattingOptions options;
        BraceStyle braceStyle = BraceStyle::Allman;
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
     * @brief Formats code triggered on typing specific characters (;, }, \n).
     * @param request Immutable on-type formatting context.
     * @return List of TextEdits for the formatted region.
     */
    std::optional<std::vector<lsp::TextEdit>> FormatOnType(const OnTypeFormattingRequest &request);

    /**
     * @brief Directly formats an AngelScript source code string.
     * @param sourceCode Source text to format.
     * @param options Formatting configuration options.
     * @param braceStyle Where a block's opening brace goes. Value braces ignore this.
     * @return Formatted source code string.
     */
    std::string FormatSourceCode(std::string_view sourceCode, const lsp::FormattingOptions &options,
                                 BraceStyle braceStyle = BraceStyle::Allman);
}
