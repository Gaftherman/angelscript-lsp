#include "analysis/completion/CompletionInternal.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CompletionFacts.h"
#include "analysis/resolution/CompletionReceiverResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief Constructs a completion item for an include path candidate.
 * @param[in] candidate Absolute candidate path.
 * @param[in] fromDirectory Origin directory for relative paths.
 * @param[in] request Completion request.
 * @param[in] quoteColumn 0-indexed column of opening quote.
 * @return Constructed completion item, or nullopt if invalid.
 */
static std::optional<CompletionEntry> CreateIncludeCompletionItem(std::string_view candidate,
                                                                  const std::filesystem::path& fromDirectory,
                                                                  const CompletionQuery& request, uint32_t quoteColumn)
{
    if (candidate.empty() ||
        utils::IncludeResolver::NormalizePath(candidate) == utils::IncludeResolver::NormalizePath(request.documentPath))
    {
        return std::nullopt;
    }

    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(std::filesystem::path(candidate), fromDirectory, ec);
    if (ec || relative.empty())
    {
        return std::nullopt;
    }

    std::string insertText = relative.generic_string();
    if (!request.implicitExtension.empty() && insertText.size() > request.implicitExtension.size() &&
        insertText.ends_with(request.implicitExtension))
    {
        insertText.resize(insertText.size() - request.implicitExtension.size());
    }

    CompletionEntry item;
    item.label = insertText;
    item.kind = CompletionEntryKind::File;
    item.detail = candidate;
    item.sortText = (insertText.starts_with("../") ? "1" : "0") + insertText;

    item.textEdit = CompletionTextEdit{request.position.line, quoteColumn, request.position.character, insertText};

    return item;
}

/**
 * @brief Completes the file path inside an `#include "..."`.
 * @param[in] request Completion request.
 * @param[in] linePrefix Line text preceding the cursor.
 * @return Optional vector of include file completion items.
 */
static std::optional<std::vector<CompletionEntry>> CompleteIncludePath(const CompletionQuery& request,
                                                                       const std::string& linePrefix)
{
    if (request.documentPath.empty() || !request.listIncludeCandidates)
    {
        return std::nullopt;
    }

    static const std::regex includePrefixRegex(R"(^[ \t]*#include[ \t]*"([^"]*)$)");
    std::smatch match;
    if (!std::regex_search(linePrefix, match, includePrefixRegex))
    {
        return std::nullopt;
    }

    const std::string typed = match[1].str();
    const auto quoteColumn = static_cast<uint32_t>(linePrefix.size() - typed.size());
    const std::filesystem::path fromDirectory = std::filesystem::path(request.documentPath).parent_path();

    std::vector<CompletionEntry> items;
    std::unordered_set<std::string> offered;

    for (const std::string& candidate : request.listIncludeCandidates())
    {
        auto maybeItem = CreateIncludeCompletionItem(candidate, fromDirectory, request, quoteColumn);
        if (maybeItem && offered.insert(maybeItem->label).second)
        {
            items.push_back(std::move(*maybeItem));
        }
    }

    return items;
}

std::optional<std::vector<CompletionEntry>> HandleIncludeOrLexicalSuppression(const CompletionQuery& request,
                                                                              const std::string& prefix)
{
    if (auto includeItems = CompleteIncludePath(request, prefix))
    {
        return includeItems;
    }
    const size_t cursorOffset = utils::LineStartOffset(request.sourceCode, request.position.line) + prefix.size();
    if (ContextAtOffset(request.sourceCode, cursorOffset) != LexicalContext::Code || IsAfterCaseLabelColon(prefix))
    {
        return std::vector<CompletionEntry>{};
    }
    return std::nullopt;
}

} // namespace angel_lsp::analysis::completion
