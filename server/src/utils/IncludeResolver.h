#pragma once

#include "utils/PreprocessorRegions.h"

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
         * @brief The same, minus every directive the preprocessor removes before compilation.
         *
         * An `#include` inside a dead `#if` is not an include. CScriptBuilder blanks the whole
         * region in its first pass and only looks for `#include` in its second, so the file is
         * never opened - measured: a missing file included from inside `#if UNDEFINED` compiles
         * (exit 0), and the identical directive one line outside does not (exit 1).
         *
         * Reporting one of those as not found is a warning about text the compiler never read.
         *
         * @param excludedLines Ranges from utils::FindExcludedLineRanges. Empty behaves exactly
         *        like the overload above.
         */
        static std::vector<IncludeDirective> ExtractIncludes(
            std::string_view sourceCode,
            const std::vector<ExcludedLineRange> &excludedLines);

        /**
         * @brief Canonicalizes a path to the single spelling the rest of the server compares against.
         *        Resolves symlinks and ".." where possible, strips Windows long-path prefixes, and
         * .       normalizes separators to forward slashes.
         * @param path The path to normalize.
         * @return Normalized path string.
         */
        static std::string NormalizePath(const std::filesystem::path &path);

        /**
         * @brief NormalizePath for a path a directory walk produced, which is most of them.
         *
         * The general form calls `weakly_canonical`, which queries the filesystem for every
         * component. That is what a path arriving from a client URI or a setting needs - it may be
         * spelled with `..`, with the wrong case, or through a symlink - and it is also nearly the
         * whole of this server's startup. Measured against a generated workspace, the include-graph
         * phase was 1016 ms of a 1025 ms scan at 200 files, and 146 ms with the call removed.
         *
         * A path from `std::filesystem::directory_iterator` needs none of that work on its last
         * component: the filesystem produced that name, so it is already the on-disk spelling. Only
         * the directory has to be canonicalised, and every file in a directory shares it - so it is
         * done once and remembered.
         *
         * Use this ONLY for a path the filesystem itself handed over. For anything a user or a
         * client spelled, use NormalizePath: the filename case really does have to be corrected
         * there, or `#include "HELPER.as"` and `helper.as` become two nodes for one file.
         */
        static std::string NormalizeWalkedPath(const std::filesystem::path &path);

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
         * @param implicitExtension Suffix to try when the path as written matches no file - `.as`,
         *        typically. Empty (the default) is AngelScript's own behaviour: CScriptBuilder
         *        opens exactly the string between the quotes and appends nothing, measured, so
         *        `#include "helper"` finds a file named `helper` and not `helper.as`.
         *
         *        Some hosts resolve the name themselves before the add-on sees it, and require the
         *        extension to be left off - Sven Co-op is the reason this parameter exists. For
         *        those, `#include "helper"` **is** `helper.as` and there is nothing to report.
         *        Which of the two is true is a fact about the host, exactly like the engine
         *        properties, so it is configuration rather than a guess.
         *
         *        Tried per directory and only after the exact name misses there, so a workspace
         *        holding both `helper` and `helper.as` resolves the way the compiler would.
         * @return Canonicalized/normalized absolute path if found and permitted, else empty string.
         */
        static std::string ResolveIncludePath(
            std::string_view includePath,
            std::string_view currentFilePath,
            const std::vector<std::string> &searchDirectories,
            const std::vector<std::string> &allowedRoots = {},
            std::string_view implicitExtension = {});

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
