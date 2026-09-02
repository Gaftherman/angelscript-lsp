#include "utils/WorkspaceScan.h"

#include "utils/Utils.h"

namespace angel_lsp::utils
{
    bool ForEachWorkspaceFile(
        const std::vector<std::string> &roots,
        const std::vector<std::string> &excludeGlobs,
        const std::function<bool()> &shouldStop,
        const std::function<void(const std::filesystem::directory_entry &)> &onFile)
    {
        const auto stopped = [&shouldStop]() { return shouldStop && shouldStop(); };

        for (const auto &root : roots)
        {
            if (stopped())
                return false;

            std::error_code ec;
            std::filesystem::recursive_directory_iterator scan(
                root, std::filesystem::directory_options::skip_permission_denied, ec);

            // A root that cannot be opened is skipped rather than iterated. Two of the three walks
            // this replaces passed an error_code and then ignored it, which leaves `scan` in a
            // state the following loop has no business reading.
            if (ec)
                continue;

            const std::filesystem::recursive_directory_iterator scanEnd;
            for (; scan != scanEnd; ++scan)
            {
                if (stopped())
                    return false;

                const std::filesystem::directory_entry &entry = *scan;

                // Pruned, not filtered: disable_recursion_pending stops the walk from entering the
                // directory at all, where a filter would still visit every file inside it.
                std::error_code dirError;
                if (entry.is_directory(dirError) &&
                    IsExcludedDirectory(entry.path().generic_string(), excludeGlobs))
                {
                    scan.disable_recursion_pending();
                    continue;
                }

                std::error_code fileError;
                if (!entry.is_regular_file(fileError) || fileError)
                    continue;

                onFile(entry);
            }
        }

        return true;
    }
}
