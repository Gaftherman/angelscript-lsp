#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace angel_lsp::test
{
    /**
     * @brief Where the `angelscript/` corpus of real scripts lives, if it is present at all.
     *
     * The corpus is ~1,061 files flattened out of some twenty third-party Sven Co-op projects. It
     * is deliberately not in this repository - it is 13 MB of other people's code, and `.gitignore`
     * has excluded it since the start - so any environment that did not put it there by hand does
     * not have it. CI is one of those, which is the whole reason this exists: the audits that walk
     * it were pinned to a compile-time path that only ever resolved on a developer's own machine.
     *
     * `ANGELLSP_CORPUS_DIR` overrides the compiled-in path, so a workflow can point at a corpus it
     * checked out or downloaded without rebuilding. An empty return means the corpus is not
     * reachable and the caller must skip rather than fail: an audit that cannot see the corpus has
     * measured nothing, and reporting that as a pass is worse than reporting it as absent.
     */
    inline const std::filesystem::path &CorpusDirectory()
    {
        static const std::filesystem::path resolved = []
        {
            std::error_code ec;

            // An explicitly set override decides the answer on its own, even when it does not
            // resolve. Falling back to the compiled path would mean a workflow whose corpus
            // checkout failed silently audits whatever happens to sit at the build-time location -
            // a different corpus, or none - and reports the result as though it had run the one it
            // was told to.
            if (const char *fromEnv = std::getenv("ANGELLSP_CORPUS_DIR"); fromEnv && *fromEnv)
            {
                const std::filesystem::path candidate(fromEnv);
                return std::filesystem::is_directory(candidate, ec) ? candidate
                                                                    : std::filesystem::path{};
            }

            const std::filesystem::path compiled(ANGELSCRIPT_CORPUS_DIR);
            if (std::filesystem::is_directory(compiled, ec))
            {
                return compiled;
            }

            return std::filesystem::path{};
        }();

        return resolved;
    }

    /** @brief True when the corpus is reachable, so an audit over it can say something. */
    inline bool CorpusIsAvailable()
    {
        return !CorpusDirectory().empty();
    }

    /**
     * @brief The one line every corpus audit prints when it has nothing to walk.
     *
     * Kept as a string rather than a MESSAGE call so each audit reports it through doctest's own
     * logging, where it lands beside that audit's name instead of at the top of the run.
     */
    inline std::string CorpusMissingMessage()
    {
        return "angelscript/ corpus not present (set ANGELLSP_CORPUS_DIR) - audit skipped, "
               "nothing was measured.";
    }
}
