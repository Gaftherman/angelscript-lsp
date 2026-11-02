#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace angel_lsp::utils
{
/**
 * @brief Walks the regular files under a set of roots, pruning directories that are excluded.
 *        Written once because it was written three times: the predefined-stub scan, the engine-profile
 * @param roots Directory paths - not URIs. A caller holding workspace folders converts first.
 * @param excludeGlobs Passed to IsExcludedDirectory.
 * @param shouldStop Polled to abort. May be empty, meaning the walk always runs to completion.
 * @param onFile Called for each regular file, in filesystem order.
 * @return True when every root was walked to the end, false when @p shouldStop ended it early.
 *        The distinction matters: one caller has progress reporting to close out on a cancel.
 */
bool ForEachWorkspaceFile(const std::vector<std::string>& roots, const std::vector<std::string>& excludeGlobs,
                          const std::function<bool()>& shouldStop,
                          const std::function<void(const std::filesystem::directory_entry&)>& onFile);
} // namespace angel_lsp::utils
