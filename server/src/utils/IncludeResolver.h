#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::utils
{
    /**
     * @brief Represents an extracted #include directive from AngelScript source code.
     */
    struct IncludeDirective
    {
        std::string rawPath;       ///< The raw file path inside the include quotes or brackets.
        size_t line = 0;           ///< 0-indexed line number in the source file where the directive occurs.
        std::string resolvedPath;  ///< Canonical/normalized filesystem path if resolved, or empty string if not found.
        bool isAngled = false;     ///< True for <path>, false for "path".
    };

    /**
     * @brief Utility class for extracting, resolving, and transitively indexing AngelScript #include directives.
     */
    class IncludeResolver
    {
    public:
        /**
         * @brief Scans source code and extracts all #include directives.
         *        Correctly skips directives appearing in single-line comments (//),
         *        multi-line comments (/ * ... * /), and string literals ("...", '...', @"...").
         * @param sourceCode The AngelScript source code to analyze.
         * @return A vector of extracted IncludeDirective structs with rawPath, line, and isAngled populated.
         */
        static std::vector<IncludeDirective> ExtractIncludes(std::string_view sourceCode);

        /**
         * @brief Resolves a single include path against the current file's directory and search directories.
         * @param includePath The raw include path from the directive.
         * @param currentFilePath The path of the file containing the include directive.
         * @param searchDirectories Ordered list of search paths configured for the workspace.
         * @return Canonicalized/normalized absolute path if found, or empty string if not found.
         */
        static std::string ResolveIncludePath(
            std::string_view includePath,
            std::string_view currentFilePath,
            const std::vector<std::string> &searchDirectories);

        /**
         * @brief Recursively discovers all resolved include files starting from rootFilePath.
         *        Guards against cyclic dependencies and diamond include patterns using visited tracking.
         * @param rootFilePath The entry-point file path to start resolution from.
         * @param searchDirectories Ordered list of search paths.
         * @param fileReader Optional callback to load file content given its resolved path.
         *                   If not provided or null, defaults to reading from disk via std::ifstream.
         * @return Ordered vector of unique resolved file paths discovered in the include tree.
         */
        static std::vector<std::string> ResolveAllIncludes(
            std::string_view rootFilePath,
            const std::vector<std::string> &searchDirectories,
            std::function<std::string(const std::string &)> fileReader = nullptr);
    };
}
