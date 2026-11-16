#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/PositionEncoding.h"
#include "utils/TransparentHash.h"
#include "utils/Utils.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp
{
/**
 * @brief Encodes a batch of ranges against the documents they belong to, indexing each document once.
 * @details A workspace result can hold thousands of ranges across a few documents, and utils::GetLine() rescans
 *          its document from byte 0 for each one. A document's LineIndex costs one pass over the whole text, so
 *          it is only built once a document has more than k_indexAfter ranges; small results keep the cheap path.
 * @param[in] enc Negotiated position encoding.
 * @param[in] findText Returns the text of the document stored under a document key, or null when unknown.
 * @param[in,out] targets Document key and the range to rewrite in place.
 */
static void EncodeRangesByDocument(utils::PositionEncoding enc,
                                   const std::function<std::shared_ptr<const std::string>(std::string_view)>& findText,
                                   const std::vector<std::pair<std::string, lsp::Range*>>& targets)
{
    constexpr size_t k_indexAfter = 4;
    ankerl::unordered_dense::map<std::string, std::pair<std::shared_ptr<const std::string>, utils::LineIndex>,
                                 TransparentStringHash, std::equal_to<>>
        documents;
    ankerl::unordered_dense::map<std::string, size_t, TransparentStringHash, std::equal_to<>> uses;
    for (const auto& [key, range] : targets)
    {
        auto it = documents.find(key);
        if (it == documents.end())
        {
            it = documents.emplace(key, std::pair{findText(key), utils::LineIndex{}}).first;
        }
        const auto& text = it->second.first;
        if (!text)
        {
            continue;
        }
        if (++uses[key] <= k_indexAfter)
        {
            codec::Encode(*text, enc, *range);
            continue;
        }
        if (it->second.second.LineCount() == 0)
        {
            it->second.second = utils::LineIndex::Build(*text);
        }
        codec::Encode(it->second.second, *text, enc, *range);
    }
}

static void EncodeSymbolsRecursive(const utils::LineIndex& lineIndex, std::string_view text,
                                   utils::PositionEncoding enc, std::vector<lsp::DocumentSymbol>& symbols)
{
    for (auto& symbol : symbols)
    {
        codec::Encode(lineIndex, text, enc, symbol.range);
        codec::Encode(lineIndex, text, enc, symbol.selectionRange);

        if (symbol.children.has_value())
        {
            EncodeSymbolsRecursive(lineIndex, text, enc, symbol.children.value());
        }
    }
}

void Server::EncodeIn(std::string_view text, lsp::Range& range) const
{
    codec::Encode(text, GetPositionEncoding(), range);
}

void Server::EncodeIn(std::string_view text, lsp::Hover& hover) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || !hover.range.has_value())
    {
        return;
    }

    codec::Encode(text, GetPositionEncoding(), hover.range.value());
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::TextEdit>& edits) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || edits.empty())
    {
        return;
    }

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& edit : edits)
    {
        codec::Encode(lineIndex, text, GetPositionEncoding(), edit.range);
    }
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentHighlight>& highlights) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || highlights.empty())
    {
        return;
    }

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& highlight : highlights)
    {
        codec::Encode(lineIndex, text, GetPositionEncoding(), highlight.range);
    }
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::FoldingRange>& ranges) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || ranges.empty())
    {
        return;
    }

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& range : ranges)
    {
        if (range.startCharacter.has_value())
        {
            const std::string_view line = lineIndex.Line(text, range.startLine);
            range.startCharacter =
                angel_lsp::utils::ByteToLspCharColumn(line, range.startCharacter.value(), GetPositionEncoding());
        }

        if (range.endCharacter.has_value())
        {
            const std::string_view line = lineIndex.Line(text, range.endLine);
            range.endCharacter =
                angel_lsp::utils::ByteToLspCharColumn(line, range.endCharacter.value(), GetPositionEncoding());
        }
    }
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentLink>& links) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || links.empty())
    {
        return;
    }

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& link : links)
    {
        codec::Encode(lineIndex, text, GetPositionEncoding(), link.range);
    }
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::InlayHint>& hints) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || hints.empty())
    {
        return;
    }

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& hint : hints)
    {
        codec::Encode(lineIndex, text, GetPositionEncoding(), hint.position);

        if (hint.textEdits.has_value())
        {
            for (auto& edit : hint.textEdits.value())
            {
                codec::Encode(lineIndex, text, GetPositionEncoding(), edit.range);
            }
        }
    }
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentSymbol>& symbols) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || symbols.empty())
    {
        return;
    }

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    EncodeSymbolsRecursive(lineIndex, text, GetPositionEncoding(), symbols);
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::CodeLens>& lenses) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || lenses.empty())
    {
        return;
    }

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& lens : lenses)
    {
        codec::Encode(lineIndex, text, GetPositionEncoding(), lens.range);
    }
}

void Server::EncodeIn(std::string_view text, lsp::PrepareRenameResult& result) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
    {
        return;
    }

    if (auto* range = std::get_if<lsp::Range>(&result))
    {
        codec::Encode(text, GetPositionEncoding(), *range);
    }
    else if (auto* placeholder = std::get_if<lsp::PrepareRenamePlaceholder>(&result))
    {
        codec::Encode(text, GetPositionEncoding(), placeholder->range);
    }
}

void Server::EncodeAcrossDocuments(std::vector<lsp::Location>& locations) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
    {
        return;
    }

    std::vector<std::pair<std::string, lsp::Range*>> targets;
    targets.reserve(locations.size());
    for (auto& location : locations)
    {
        targets.emplace_back(DocumentKey(location.uri.toString()), &location.range);
    }
    EncodeRangesByDocument(
        GetPositionEncoding(), [this](std::string_view key) { return FindDocumentText(key); }, targets);
}

void Server::EncodeAcrossDocuments(std::vector<lsp::SymbolInformation>& symbols) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
    {
        return;
    }

    std::vector<std::pair<std::string, lsp::Range*>> targets;
    targets.reserve(symbols.size());
    for (auto& symbol : symbols)
    {
        targets.emplace_back(DocumentKey(symbol.location.uri.toString()), &symbol.location.range);
    }
    EncodeRangesByDocument(
        GetPositionEncoding(), [this](std::string_view key) { return FindDocumentText(key); }, targets);
}

void Server::EncodeAcrossDocuments(lsp::WorkspaceEdit& edit) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
    {
        return;
    }

    if (edit.changes.has_value())
    {
        for (auto& [uri, edits] : edit.changes.value())
        {
            if (auto text = FindDocumentText(uri.toString()))
            {
                EncodeIn(*text, edits);
            }
        }
    }

    if (edit.documentChanges.has_value())
    {
        for (auto& docChange : edit.documentChanges.value())
        {
            if (std::holds_alternative<lsp::TextDocumentEdit>(docChange))
            {
                auto& docEdit = std::get<lsp::TextDocumentEdit>(docChange);
                if (auto text = FindDocumentText(DocumentKey(docEdit.textDocument.uri.toString())))
                {
                    for (auto& e : docEdit.edits)
                    {
                        if (std::holds_alternative<lsp::TextEdit>(e))
                        {
                            auto& te = std::get<lsp::TextEdit>(e);
                            codec::Encode(*text, GetPositionEncoding(), te.range);
                        }
                        else if (std::holds_alternative<lsp::AnnotatedTextEdit>(e))
                        {
                            auto& ate = std::get<lsp::AnnotatedTextEdit>(e);
                            codec::Encode(*text, GetPositionEncoding(), ate.range);
                        }
                        else if (std::holds_alternative<lsp::SnippetTextEdit>(e))
                        {
                            auto& ste = std::get<lsp::SnippetTextEdit>(e);
                            codec::Encode(*text, GetPositionEncoding(), ste.range);
                        }
                    }
                }
            }
        }
    }
}

void Server::EncodeRangesIn(std::string_view uri, std::vector<lsp::Range>& ranges) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
    {
        return;
    }

    if (auto text = FindDocumentText(uri))
    {
        for (auto& range : ranges)
        {
            codec::Encode(*text, GetPositionEncoding(), range);
        }
    }
}

void Server::EncodeAcrossDocuments(std::vector<lsp::CodeAction>& actions) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
    {
        return;
    }

    for (auto& action : actions)
    {
        if (action.edit.has_value())
        {
            EncodeAcrossDocuments(action.edit.value());
        }
    }
}
} // namespace angel_lsp
