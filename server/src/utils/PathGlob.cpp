#include "utils/PathGlob.h"

#include <algorithm>
#include <cctype>

namespace angel_lsp::utils
{
/**
 * @brief Normalizes slash spelling and removes leading separators and dot segments.
 * @param[in] path Pattern or path to normalize.
 * @return Text using forward slashes, starting at its first named segment.
 */
static std::string NormalizeGlobText(std::string_view path)
{
    std::string text(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    std::string_view relative(text);
    while (relative.starts_with("./") || relative.starts_with('/'))
    {
        relative.remove_prefix(relative.starts_with("./") ? 2 : 1);
    }
    text.erase(0, text.size() - relative.size());
    return text;
}

/**
 * @brief Compares two characters according to the platform's path case policy.
 * @param[in] left Character in a path or pattern.
 * @param[in] right Character in the other path or pattern.
 * @return True when the two characters match.
 */
static bool EqualPathCharacter(char left, char right)
{
#if defined(_WIN32)
    return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
#else
    return left == right;
#endif
}

PathGlob::PathGlob(std::string_view pattern)
{
    const auto normalized = NormalizeGlobText(pattern);
    m_matchFileName = normalized.find('/') == std::string::npos;
    std::string_view rest(normalized);
    while (!rest.empty())
    {
        if (rest.starts_with("**/"))
        {
            m_tokens.push_back({Kind::Directories});
            rest.remove_prefix(3);
        }
        else if (rest.starts_with("**"))
        {
            m_tokens.push_back({Kind::Path});
            rest.remove_prefix(2);
        }
        else
        {
            const char c = rest.front();
            m_tokens.push_back(c == '*'   ? Token{Kind::Segment}
                               : c == '?' ? Token{Kind::Character}
                                          : Token{Kind::Literal, c});
            rest.remove_prefix(1);
        }
    }
    if (normalized.ends_with('/'))
    {
        m_tokens.push_back({Kind::Path});
    }
}

bool PathGlob::Matches(std::string_view relativePath) const
{
    const auto normalized = NormalizeGlobText(relativePath);
    std::string_view text(normalized);
    if (m_matchFileName)
    {
        const auto slash = text.find_last_of('/');
        if (slash != std::string_view::npos)
        {
            text.remove_prefix(slash + 1);
        }
    }
    if (m_tokens.empty() || text.empty())
    {
        return false;
    }

    // The row tracks which prefixes match the tokens processed so far.
    std::vector<char> matched(text.size() + 1, 0);
    std::vector<char> next(matched.size(), 0);
    matched.front() = 1;
    for (const auto& token : m_tokens)
    {
        const bool star = token.kind == Kind::Segment || token.kind == Kind::Path;
        const bool directories = token.kind == Kind::Directories;
        next.front() = (star || directories) && matched.front();
        bool canSkipDirectories = false;
        for (size_t i = 1; i <= text.size(); ++i)
        {
            const char c = text[i - 1];
            switch (token.kind)
            {
            case Kind::Literal:
                next[i] = matched[i - 1] && EqualPathCharacter(c, token.literal);
                break;
            case Kind::Character:
                next[i] = matched[i - 1] && c != '/';
                break;
            case Kind::Segment:
            case Kind::Path:
                next[i] = matched[i] || (next[i - 1] && (token.kind == Kind::Path || c != '/'));
                break;
            case Kind::Directories:
                canSkipDirectories = canSkipDirectories || matched[i - 1];
                next[i] = matched[i] || (canSkipDirectories && c == '/');
                break;
            }
        }
        matched.swap(next);
    }
    return matched.back() != 0;
}

std::optional<std::string_view> RelativeToWorkspaceRoot(std::string_view path, std::span<const std::string> roots)
{
    size_t deepest = 0;
    for (std::string_view root : roots)
    {
        while (root.size() > 1 && root.ends_with('/'))
        {
            root.remove_suffix(1);
        }
        if (root.size() <= deepest || root.size() >= path.size() ||
            (!root.ends_with('/') && path[root.size()] != '/') ||
            !std::equal(root.begin(), root.end(), path.begin(), EqualPathCharacter))
        {
            continue;
        }
        deepest = root.size();
    }
    if (deepest == 0)
    {
        return std::nullopt;
    }
    return path.substr(deepest + (path[deepest] == '/' ? 1 : 0));
}
} // namespace angel_lsp::utils
