#pragma once

#include "utils/PreprocessorRegions.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::utils
{
/**
 * @brief Information about an include directive located on a single line.
 */
struct SingleLineIncludeInfo
{
    size_t hashPos{0};
    size_t openDelim{0};
    size_t closeDelim{0};
    std::string rawPath;
    bool isAngled{false};
};

/**
 * @brief Extracted #include directive from AngelScript source code.
 */
struct IncludeDirective
{
    std::string rawPath;      ///< The raw file path inside the include quotes or brackets.
    size_t line = 0;          ///< 0-indexed line number in the source file where the directive occurs.
    std::string resolvedPath; ///< Canonical/normalized filesystem path if resolved, or empty string if not found.
    bool isAngled = false;    ///< True for <path>, false for "path".
};

/**
 * @brief Immutable inputs for evaluating conditional includes.
 */
struct IncludePreprocessor
{
    std::shared_ptr<const ankerl::unordered_dense::set<std::string>> definedWords;
    PreprocessorFeatures features;

    [[nodiscard]] std::vector<ExcludedLineRange> ExcludedLines(std::string_view source) const;
};

/**
 * @brief Inputs for resolving all active includes from an entry file.
 */
struct IncludeTraversalRequest
{
    std::string_view entry;
    std::span<const std::string> searchDirectories;
    std::span<const std::string> allowedRoots;
    std::function<std::string(const std::string&)> fileReader;
    std::string_view implicitExtension;
    IncludePreprocessor preprocessor;
};

/**
 * @brief Workspace include configuration a document is resolved against: where to search, where resolution may
 *        land, and which suffix to retry with.
 * @warning Non-owning: the caller keeps the directory lists and extension alive while the scope is in use.
 */
struct IncludeSearchScope
{
    std::span<const std::string> searchDirectories{}; ///< Ordered list of search paths configured for the workspace.
    std::span<const std::string> allowedRoots{};      ///< Confinement roots; empty means unconfined.
    std::string_view implicitExtension{};             ///< Suffix fallback to try (e.g., ".as"), or empty for none.
};

/**
 * @brief Parameters for resolving an AngelScript #include directive.
 */
struct IncludeResolveRequest
{
    std::string_view includePath{};                   ///< The raw include path from the directive.
    std::string_view currentFilePath{};               ///< Path of the file containing the include directive.
    std::span<const std::string> searchDirectories{}; ///< Ordered list of search paths configured for the workspace.
    std::span<const std::string> allowedRoots{};      ///< Confinement roots; empty means unconfined.
    std::string_view implicitExtension{};             ///< Suffix fallback to try (e.g., ".as").
};

/**
 * @brief Utility class for extracting, resolving, and transitively indexing AngelScript #include directives.
 */
class IncludeResolver
{
  public:
    /**
     * @brief Parses an #include directive from a single line of text at an optional character position.
     * @param[in] line Line text to scan.
     * @param[in] character Cursor column index (0-indexed). If string_view::npos, position bounds check is skipped.
     * @return Extracted SingleLineIncludeInfo if found and within position, or std::nullopt.
     */
    static std::optional<SingleLineIncludeInfo> ParseSingleLineInclude(std::string_view line,
                                                                       size_t character = std::string_view::npos);

    /**
     * @brief Scans source code and extracts all #include directives.
     *        Correctly skips directives appearing in single-line comments (//),
     * @param[in] sourceCode The AngelScript source code to analyze.
     * @return A vector of extracted IncludeDirective structs with rawPath, line, and isAngled populated.
     */
    static std::vector<IncludeDirective> ExtractIncludes(std::string_view sourceCode);

    /**
     * @brief The same, minus every directive the preprocessor removes before compilation.
     *        An `#include` inside a dead `#if` is not an include. CScriptBuilder blanks the whole
     * @param[in] excludedLines Ranges from utils::FindExcludedLineRanges. Empty behaves exactly
     *        like the overload above.
     */
    static std::vector<IncludeDirective> ExtractIncludes(std::string_view sourceCode,
                                                         const std::vector<ExcludedLineRange>& excludedLines);

    /**
     * @brief Canonicalizes a path to the single spelling the rest of the server compares against.
     *        Resolves symlinks and ".." where possible, strips Windows long-path prefixes, and
     * @param[in] path The path to normalize.
     * @return Normalized path string.
     */
    static std::string NormalizePath(const std::filesystem::path& path);

    /**
     * @brief NormalizePath for a path a directory walk produced, which is most of them.
     *        The general form calls `weakly_canonical`, which queries the filesystem for every
     */
    static std::string NormalizeWalkedPath(const std::filesystem::path& path);

    /**
     * @brief Forgets every remembered directory canonicalisation.
     *        The cache behind NormalizeWalkedPath trades a stale answer for a much faster startup, and
     */
    static void ForgetCanonicalDirectories();

    /**
     * @brief True when a normalized path lies inside one of the allowed root directories.
     *        The confinement check behind every resolve. `#include` accepts whatever text sits between
     * @param[in] normalizedPath The normalized path to verify.
     * @param[in] allowedRoots Roots to test against. **Empty means unconfined**, which is what a
     *        caller with no workspace context (a unit test, a bare library user) gets.
     * @return True if normalizedPath lies inside allowedRoots or allowedRoots is empty, false otherwise.
     */
    static bool IsWithinRoots(const std::string& normalizedPath, std::span<const std::string> allowedRoots);

    /**
     * @brief Configures whether empty allowedRoots permits unconfined resolution (unit test harness mode).
     * @param[in] enabled True to enable unconfined test mode, false for strict production confinement.
     */
    static void SetIsolatedHarnessMode(bool enabled);

    /**
     * @brief Queries whether unconfined unit test harness mode is currently active.
     * @return True if unconfined harness mode is active.
     */
    static bool IsIsolatedHarnessMode();

    /**
     * @brief Resolves a single include path against the current file's directory and search directories.
     * @param[in] request The bundled include resolution parameters.
     * @return Canonicalized/normalized absolute path if found and permitted, else empty string.
     */
    static std::string ResolveIncludePath(const IncludeResolveRequest& request);

    /**
     * @brief Resolves a single include path against the current file's directory and search directories.
     * @param[in] includePath The raw include path from the directive.
     * @param[in] currentFilePath The path of the file containing the include directive.
     * @param[in] searchDirectories Ordered list of search paths configured for the workspace.
     * @param[in] allowedRoots Confinement roots; see IsWithinRoots. Empty (the default) resolves
     *        without confinement, which is what unit tests and library callers want. The server
     * @return Canonicalized/normalized absolute path if found and permitted, else empty string.
     */
    static std::string ResolveIncludePath(std::string_view includePath, std::string_view currentFilePath,
                                          const std::vector<std::string>& searchDirectories,
                                          const std::vector<std::string>& allowedRoots = {});

    /**
     * @brief Resolves an include directive path strictly contained within the workspace root.
     * @param[in] workspaceRoot The root directory of the workspace.
     * @param[in] currentFilePath The absolute path of the file containing the include.
     * @param[in] includePath The raw include path string from the directive.
     * @return The canonical path if valid and contained within workspace, otherwise an empty path.
     */
    static std::filesystem::path resolveInclude(const std::filesystem::path& workspaceRoot,
                                                const std::filesystem::path& currentFilePath,
                                                std::string_view includePath);

    /**
     * @brief Recursively discovers all resolved include files starting from rootFilePath.
     *        Guards against cyclic dependencies and diamond include patterns using visited tracking.
     * @param[in] rootFilePath The entry-point file path to start resolution from.
     * @param[in] searchDirectories Ordered list of search paths.
     * @param[in] fileReader Optional callback to load file content given its resolved path.
     *        If not provided or null, defaults to reading from disk via std::ifstream.
     * @return Ordered vector of unique resolved file paths discovered in the include tree.
     */
    static std::vector<std::string> ResolveAllIncludes(const IncludeTraversalRequest& request);

    static std::vector<std::string>
    ResolveAllIncludes(std::string_view rootFilePath, const std::vector<std::string>& searchDirectories,
                       std::function<std::string(const std::string&)> fileReader = nullptr,
                       const std::vector<std::string>& allowedRoots = {});
};

/**
 * @brief Tracking state during include directive line scanning and path normalization.
 */
struct IncludeScanState
{
    size_t index{0};
    size_t currentLine{0};
    bool atLineStart{true};
};
} // namespace angel_lsp::utils
