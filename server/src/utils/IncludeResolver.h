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
         * @brief Canonicalizes a path to the single spelling the rest of the server compares against.
         *        Resolves symlinks and ".." where possible, strips Windows long-path prefixes, and
         * .       normalizes separators to forward slashes.
         * @param path The path to normalize.
         * @return Normalized path string.
         */
        static std::string NormalizePath(const std::filesystem::path &path);

        /**
         * @brief True when a normalized path lies inside one of the allowed root directories.
         *
         * The confinement check behind every resolve. `#include` accepts whatever text sits between
         * the quotes, so without this an absolute path or enough `../` steps reads any file the
         * server process can - and because included files are indexed and their text retained, the
         * contents come back to the client through hover, definition and references. Opening an
         * untrusted repository was enough to trigger it.
         *
         * Compare only paths that have already been through NormalizePath: it applies
         * weakly_canonical, so `..` is collapsed and symlinks are resolved *before* the prefix test
         * and cannot be used to step outside a root that appears to contain them.
         *
         * Matching is per path component - "/w/lib" does not contain "/w/library" - and
         * case-insensitive on Windows, where the same file has many spellings.
         *
         * @param allowedRoots Roots to test against. **Empty means unconfined**, which is what a
         *        caller with no workspace context (a unit test, a bare library user) gets.
         */
        static bool IsWithinRoots(const std::string &normalizedPath,
                                  const std::vector<std::string> &allowedRoots);

        /**
         * @brief Resolves a single include path against the current file's directory and search directories.
         * @param includePath The raw include path from the directive.
         * @param currentFilePath The path of the file containing the include directive.
         * @param searchDirectories Ordered list of search paths configured for the workspace.
         * @param allowedRoots Confinement roots; see IsWithinRoots. Empty (the default) resolves
         *        without confinement, which is what unit tests and library callers want. The server
         *        always passes its workspace folders and search directories.
         * @return Canonicalized/normalized absolute path if found and permitted, else empty string.
         */
        static std::string ResolveIncludePath(
            std::string_view includePath,
            std::string_view currentFilePath,
            const std::vector<std::string> &searchDirectories,
            const std::vector<std::string> &allowedRoots = {});

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
            std::function<std::string(const std::string &)> fileReader = nullptr,
            const std::vector<std::string> &allowedRoots = {});
    };
}
