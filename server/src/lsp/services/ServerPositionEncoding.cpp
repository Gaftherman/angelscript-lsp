#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <lsp/strmap.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp
{
/**
 * @brief Creates an encoder for a batch of ranges belonging to multiple documents.
 * @param[in] enc Negotiated position encoding.
 * @param[in] findText Returns the text of the document stored under a document key, or null when unknown.
 * @param[in] activeText Request snapshot that may also be addressed through a label URI alias.
 * @param[in] activeIndex Line index shared with the request document.
 * @return Callable that encodes a range in place and returns false when its source is unavailable.
 */
template <typename Key = std::string, typename FindText>
static auto MakeDocumentRangeEncoder(utils::PositionEncoding enc, FindText findText,
                                     const std::string* activeText = nullptr,
                                     const utils::LineIndex* activeIndex = nullptr)
{
    using Source = std::pair<std::shared_ptr<const std::string>, codec::DocumentPositionEncoder>;
    using Documents = ankerl::unordered_dense::map<Key, Source, lsp::TransparentStrHash, std::equal_to<>>;
    return [enc, findText = std::move(findText), activeText, activeIndex,
            documents = std::optional<Documents>{}](const auto& key, lsp::Range& range) mutable
    {
        if (!documents)
        {
            documents.emplace();
        }
        auto it = documents->find(key);
        if (it == documents->end())
        {
            auto text = findText(key);
            codec::DocumentPositionEncoder encoder(text ? std::string_view(*text) : std::string_view{}, enc);
            it = documents->try_emplace(Key(key), std::move(text), std::move(encoder)).first;
        }
        auto& [text, encoder] = it->second;
        if (!text)
        {
            return false;
        }
        if (enc != utils::PositionEncoding::Utf8)
        {
            if (activeIndex && text.get() == activeText)
            {
                codec::Encode(*activeIndex, *text, enc, range);
            }
            else
            {
                encoder.Prepare(1);
                encoder.Encode(range);
            }
        }
        return true;
    };
}

template <typename Item, typename Encode, typename LocationOf>
static void EncodeLocatedItems(std::vector<Item>& items, Encode encode, LocationOf locationOf)
{
    auto output = items.begin();
    for (auto item = items.begin(); item != items.end(); ++item)
    {
        auto& location = locationOf(*item);
        if (encode(Server::DocumentKey(location.uri.toString()), location.range))
        {
            if (output != item)
            {
                *output = std::move(*item);
            }
            ++output;
        }
    }
    items.erase(output, items.end());
}

std::shared_ptr<const std::string> Server::FindRangeSource(std::string_view uri, bool readDisk) const
{
    if (auto text = FindDocumentText(uri))
    {
        return text;
    }
    if (m_workspaceStore)
    {
        if (auto document = m_workspaceStore->GetPreindexedDocument(DocumentKey(uri)))
        {
            return std::shared_ptr<const std::string>(document, &document->text);
        }
    }
    if (!readDisk || !uri.starts_with("file://"))
    {
        return nullptr;
    }
    const std::filesystem::path path(utils::UriToPath(uri));
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
    {
        return nullptr;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return nullptr;
    }
    return std::make_shared<const std::string>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
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
    const auto encoding = GetPositionEncoding();
    for (auto& range : ranges)
    {
        if (range.startCharacter.has_value())
        {
            lsp::Position start{range.startLine, *range.startCharacter};
            codec::Encode(lineIndex, text, encoding, start);
            range.startCharacter = start.character;
        }

        if (range.endCharacter.has_value())
        {
            lsp::Position end{range.endLine, *range.endCharacter};
            codec::Encode(lineIndex, text, encoding, end);
            range.endCharacter = end.character;
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

void Server::EncodeIn(const OpenDocument& doc, std::vector<lsp::InlayHint>& hints) const
{
    const auto encoding = GetPositionEncoding();
    if (encoding == utils::PositionEncoding::Utf8 || hints.empty())
    {
        return;
    }

    const std::string_view text = *doc.text;
    const auto index = utils::LineIndex::Build(text);
    std::optional<lsp::Uri> activeUri;
    auto encodeDestination = MakeDocumentRangeEncoder<lsp::Uri>(
        encoding,
        [this, &doc](const lsp::Uri& uri)
        {
            const auto key = DocumentKey(uri.toString());
            if (key == doc.uri)
            {
                return doc.predefinedHandle ? doc.predefinedHandle
                                            : std::shared_ptr<const std::string>(doc.docHandle, doc.text);
            }
            return FindRangeSource(key);
        },
        doc.text, &index);
    for (auto& hint : hints)
    {
        codec::Encode(index, text, encoding, hint.position);

        if (hint.textEdits)
        {
            for (auto& edit : *hint.textEdits)
            {
                codec::Encode(index, text, encoding, edit.range);
            }
        }
        auto* parts = std::get_if<std::vector<lsp::InlayHintLabelPart>>(&hint.label);
        if (!parts)
        {
            continue;
        }
        for (auto& part : *parts)
        {
            auto& location = part.location;
            if (!location)
            {
                continue;
            }
            if (!activeUri)
            {
                activeUri.emplace(lsp::Uri::parse(doc.uri));
            }
            if (location->uri == *activeUri)
            {
                codec::Encode(index, text, encoding, location->range);
            }
            else if (!encodeDestination(location->uri, location->range))
            {
                location.reset();
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

    auto encode =
        MakeDocumentRangeEncoder(GetPositionEncoding(), [this](std::string_view uri) { return FindRangeSource(uri); });
    EncodeLocatedItems(locations, std::move(encode),
                       [](lsp::Location& location) -> lsp::Location& { return location; });
}

void Server::EncodeAcrossDocuments(std::vector<lsp::SymbolInformation>& symbols) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
    {
        return;
    }

    auto encode =
        MakeDocumentRangeEncoder(GetPositionEncoding(), [this](std::string_view uri) { return FindRangeSource(uri); });
    EncodeLocatedItems(symbols, std::move(encode),
                       [](lsp::SymbolInformation& symbol) -> lsp::Location& { return symbol.location; });
}

void Server::EncodeRangesIn(std::string_view uri, std::vector<lsp::Range>& ranges) const
{
    auto encode = MakeDocumentRangeEncoder(GetPositionEncoding(),
                                           [this](std::string_view key) { return FindRangeSource(key, true); });
    for (auto& range : ranges)
    {
        if (!encode(uri, range))
        {
            ranges.clear();
            return;
        }
    }
}

} // namespace angel_lsp
