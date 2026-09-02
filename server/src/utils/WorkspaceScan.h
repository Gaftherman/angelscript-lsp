#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace angel_lsp::utils
{
    /**
     * @brief Walks the regular files under a set of roots, pruning directories that are excluded.
     *
     * Written once because it was written three times: the predefined-stub scan, the engine-profile
     * detection scan and WorkspaceIncludeGraph::Build were the same twenty lines with different
     * bodies. Their comments record the same bug being found and fixed separately in each - a
     * permission-protected directory ending the walk, and a `build/` tree being filtered file by
     * file instead of pruned - and the third copy had picked up a check the other two never got.
     *
     * What the walk guarantees, in one place:
     *
     *  - `skip_permission_denied`, so one unreadable directory does not cost the caller the rest of
     *    the tree. A root that cannot be opened at all is skipped, not thrown from; the Server
     *    copies never checked that error_code and would iterate an invalid range.
     *  - Excluded directories are *pruned* with disable_recursion_pending, not filtered. A filter
     *    still descends into `node_modules` to reject every file inside it one at a time.
     *  - @p shouldStop is polled before each root and before each entry, so a cancelled scan stops
     *    inside the filesystem walk rather than after it.
     *
     * @param roots Directory paths - not URIs. A caller holding workspace folders converts first.
     * @param excludeGlobs Passed to IsExcludedDirectory.
     * @param shouldStop Polled to abort. May be empty, meaning the walk always runs to completion.
     * @param onFile Called for each regular file, in filesystem order.
     * @return True when every root was walked to the end, false when @p shouldStop ended it early.
     *         The distinction matters: one caller has progress reporting to close out on a cancel.
     */
    bool ForEachWorkspaceFile(
        const std::vector<std::string> &roots,
        const std::vector<std::string> &excludeGlobs,
        const std::function<bool()> &shouldStop,
        const std::function<void(const std::filesystem::directory_entry &)> &onFile);
}
