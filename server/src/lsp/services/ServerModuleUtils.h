#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace angel_lsp
{
inline bool NormalizedPathsEqual(std::string_view a, std::string_view b)
{
    if (a == b)
    {
        return true;
    }
#if defined(_WIN32)
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
#else
    return false;
#endif
}

inline bool PathIsInside(std::string_view normalizedPath, std::string_view normalizedDirectory)
{
    if (normalizedDirectory.empty() || normalizedPath.size() <= normalizedDirectory.size() ||
        normalizedPath[normalizedDirectory.size()] != '/')
    {
        return false;
    }

    return NormalizedPathsEqual(normalizedPath.substr(0, normalizedDirectory.size()), normalizedDirectory);
}
} // namespace angel_lsp
