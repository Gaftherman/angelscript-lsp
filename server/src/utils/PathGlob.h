#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::utils
{
/**
 * @brief Compiled glob for root-relative file paths.
 *
 * @note `*` and `?` stay within one segment; `**` crosses separators.
 * @note A trailing separator selects descendants; a pattern without separators matches the file name at any depth.
 * @note Both slash styles are accepted; case is ignored on Windows.
 */
class PathGlob
{
  public:
    /**
     * @brief Compiles a user-supplied pattern once for repeated matching.
     * @param[in] pattern File path pattern. An empty pattern matches nothing.
     */
    explicit PathGlob(std::string_view pattern);

    /**
     * @brief Checks whether the file matches this glob.
     * @param[in] relativePath File path relative to a workspace folder.
     * @return True when the pattern consumes the entire path or file name, as applicable.
     */
    [[nodiscard]] bool Matches(std::string_view relativePath) const;

  private:
    enum class Kind
    {
        Literal,
        Character,
        Segment,
        Path,
        Directories
    };
    struct Token
    {
        Kind kind;
        char literal = '\0';
    };

    std::vector<Token> m_tokens;
    bool m_matchFileName = false;
};

/**
 * @brief Finds a file path relative to the deepest workspace root containing it.
 * @param[in] path Normalized absolute path with forward slashes.
 * @param[in] roots Normalized absolute workspace paths with forward slashes.
 * @return A view into the file path, or std::nullopt if the file is outside the workspace.
 */
[[nodiscard]] std::optional<std::string_view> RelativeToWorkspaceRoot(std::string_view path,
                                                                      std::span<const std::string> roots);
} // namespace angel_lsp::utils
