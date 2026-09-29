#include "utils/WorkspaceScan.h"

#include "utils/Utils.h"
#include <ankerl/unordered_dense.h>

namespace angel_lsp::utils
{
namespace
{
constexpr int k_maxScanDepth = 32;

bool ShouldSkipEntry(const std::filesystem::directory_entry& entry, const std::vector<std::string>& excludeGlobs,
                     ankerl::unordered_dense::set<std::string>& visitedSymlinks,
                     std::filesystem::recursive_directory_iterator& scan)
{
    if (scan.depth() > k_maxScanDepth)
    {
        scan.disable_recursion_pending();
        return true;
    }

    std::error_code dirError;
    if (entry.is_directory(dirError) && !dirError)
    {
        if (IsExcludedDirectory(entry.path().generic_string(), excludeGlobs))
        {
            scan.disable_recursion_pending();
            return true;
        }

        std::error_code symError;
        if (entry.is_symlink(symError) && !symError)
        {
            std::error_code canonError;
            const auto target = std::filesystem::weakly_canonical(entry.path(), canonError);
            if (!canonError && !visitedSymlinks.insert(target.string()).second)
            {
                scan.disable_recursion_pending();
                return true;
            }
        }
    }
    return false;
}
} // namespace

bool ForEachWorkspaceFile(const std::vector<std::string>& roots, const std::vector<std::string>& excludeGlobs,
                          const std::function<bool()>& shouldStop,
                          const std::function<void(const std::filesystem::directory_entry&)>& onFile)
{
    const auto stopped = [&shouldStop]() { return shouldStop && shouldStop(); };
    ankerl::unordered_dense::set<std::string> visitedSymlinks;

    for (const auto& root : roots)
    {
        if (stopped())
            return false;

        std::error_code ec;
        const auto options = std::filesystem::directory_options::skip_permission_denied |
                             std::filesystem::directory_options::follow_directory_symlink;
        std::filesystem::recursive_directory_iterator scan(root, options, ec);

        if (ec)
            continue;

        const std::filesystem::recursive_directory_iterator scanEnd;
        for (; scan != scanEnd; ++scan)
        {
            if (stopped())
                return false;

            const std::filesystem::directory_entry& entry = *scan;

            if (ShouldSkipEntry(entry, excludeGlobs, visitedSymlinks, scan))
                continue;

            std::error_code fileError;
            if (!entry.is_regular_file(fileError) || fileError)
                continue;

            onFile(entry);
        }
    }

    return true;
}
} // namespace angel_lsp::utils
