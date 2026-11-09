#include "utils/RegexCache.h"

#include "utils/TransparentHash.h"

#include <ankerl/unordered_dense.h>
#include <optional>
#include <regex>
#include <string>

namespace angel_lsp::utils
{
bool MatchesCachedRegex(std::string_view candidate, std::string_view pattern)
{
    if (pattern.empty() || candidate.empty())
    {
        return false;
    }
    thread_local ankerl::unordered_dense::map<std::string, std::optional<std::regex>, TransparentStringHash,
                                              std::equal_to<>>
        cache;
    auto it = cache.find(pattern);
    if (it == cache.end())
    {
        try
        {
            it = cache.emplace(std::string(pattern), std::regex(pattern.begin(), pattern.end())).first;
        }
        catch (const std::regex_error&)
        {
            it = cache.emplace(std::string(pattern), std::nullopt).first;
        }
    }
    return it->second.has_value() && std::regex_match(candidate.begin(), candidate.end(), *it->second);
}
} // namespace angel_lsp::utils
