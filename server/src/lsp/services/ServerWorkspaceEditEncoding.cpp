#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/TransparentHash.h"

#include <ankerl/unordered_dense.h>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp
{
/**
 * @brief Accesses the range of a plain edit or an edit with protocol metadata.
 * @param[in,out] edit Text edit or document-edit variant.
 * @return The byte range stored in the edit.
 */
template <typename Edit> static lsp::Range& EditRange(Edit& edit)
{
    if constexpr (requires { edit.range; })
    {
        return edit.range;
    }
    else
    {
        return std::visit([](auto& item) -> lsp::Range& { return item.range; }, edit);
    }
}

/**
 * @brief Validates and encodes all ranges in a single document edit.
 * @param[in,out] edits Text edits sharing one source snapshot.
 * @param[in] text Source snapshot used to produce the byte ranges.
 * @param[in] encoding Negotiated client position encoding.
 * @return True if all ranges were converted. On failure the edits remain unchanged.
 */
template <typename Edits>
static bool TryEncodeSingleDocument(Edits& edits, std::string_view text, utils::PositionEncoding encoding)
{
    codec::DocumentPositionEncoder encoder(text, encoding);
    if (edits.size() == 1)
    {
        return encoder.TryEncode(EditRange(edits.front()));
    }

    encoder.Prepare(edits.size());
    const auto encodeAndCommit = [&](std::span<lsp::Range> converted)
    {
        for (size_t i = 0; i < edits.size(); ++i)
        {
            converted[i] = EditRange(edits[i]);
            if (!encoder.TryEncode(converted[i]))
            {
                return false;
            }
        }
        for (size_t i = 0; i < edits.size(); ++i)
        {
            EditRange(edits[i]) = converted[i];
        }
        return true;
    };
    std::array<lsp::Range, 4> inlineRanges;
    if (edits.size() <= inlineRanges.size())
    {
        return encodeAndCommit(inlineRanges);
    }
    std::vector<lsp::Range> overflow(edits.size());
    return encodeAndCommit(overflow);
}

bool Server::TryEncodeWorkspaceEdit(lsp::WorkspaceEdit& edit, const OpenDocument* active) const
{
    if (active)
    {
        ActiveEditDocument source{*active, lsp::Uri::parse(active->uri), {*active->text, GetPositionEncoding()}};
        return EncodeWorkspaceEdit(edit, &source);
    }

    const auto encodeUnversioned = [this](const lsp::Uri& uri, auto& edits)
    {
        if (edits.empty())
        {
            return true;
        }
        const auto text = FindRangeSource(uri.toString());
        return text && TryEncodeSingleDocument(edits, *text, GetPositionEncoding());
    };

    if (edit.changes && edit.changes->size() == 1 && (!edit.documentChanges || edit.documentChanges->empty()))
    {
        auto& [uri, edits] = *edit.changes->begin();
        return encodeUnversioned(uri, edits);
    }
    if ((!edit.changes || edit.changes->empty()) && edit.documentChanges && edit.documentChanges->size() == 1)
    {
        auto* target = std::get_if<lsp::TextDocumentEdit>(&edit.documentChanges->front());
        if (!target)
        {
            return true;
        }
        const auto& id = target->textDocument;
        if (id.version.isNull())
        {
            return encodeUnversioned(id.uri, target->edits);
        }
        const auto open = GetDocumentStore().GetDocument(DocumentKey(id.uri.toString()));
        return open && open->version == id.version.value() &&
               (target->edits.empty() || TryEncodeSingleDocument(target->edits, open->text, GetPositionEncoding()));
    }
    return EncodeWorkspaceEdit(edit, nullptr);
}

bool Server::EncodeWorkspaceEdit(lsp::WorkspaceEdit& edit, ActiveEditDocument* active) const
{
    const auto encoding = GetPositionEncoding();
    using Source = std::pair<std::shared_ptr<const std::string>, codec::DocumentPositionEncoder>;
    using Documents = ankerl::unordered_dense::map<std::string, Source, TransparentStringHash, std::equal_to<>>;
    std::optional<Documents> documents;
    using ConvertedRange = std::pair<lsp::Range*, lsp::Range>;
    std::array<ConvertedRange, 4> inlineRanges;
    size_t inlineCount = 0;
    std::vector<ConvertedRange> overflow;

    const auto encodeDocument =
        [&](const lsp::Uri& uri, auto& edits, const lsp::OptionalVersionedTextDocumentIdentifier* id)
    {
        const bool versioned = id && !id->version.isNull();
        if (edits.empty() && !versioned)
        {
            return true;
        }
        const bool sameUri = active && uri == active->uri;
        auto canonical = sameUri ? std::string{} : DocumentKey(uri.toString());
        const bool activeTarget = sameUri || (active && canonical == active->doc.uri);
        const std::string_view key = sameUri ? std::string_view(active->doc.uri) : std::string_view(canonical);
        std::shared_ptr<const document::Document> versionedSource;
        if (versioned)
        {
            versionedSource = GetDocumentStore().GetDocument(key);
            if (!versionedSource || versionedSource->version != id->version.value())
            {
                return false;
            }
        }
        if (edits.empty())
        {
            return true;
        }

        auto* encoder = activeTarget ? &active->encoder : nullptr;
        const std::string* text = encoder ? active->doc.text : nullptr;
        if (!encoder)
        {
            if (!documents)
            {
                documents.emplace();
            }
            auto found = documents->find(key);
            if (found == documents->end())
            {
                auto source = versionedSource
                                  ? std::shared_ptr<const std::string>(versionedSource, &versionedSource->text)
                                  : FindRangeSource(key);
                if (!source)
                {
                    return false;
                }
                codec::DocumentPositionEncoder other(*source, encoding);
                found = documents->try_emplace(std::move(canonical), std::move(source), std::move(other)).first;
            }
            text = found->second.first.get();
            encoder = &found->second.second;
        }
        if (versionedSource && text != &versionedSource->text)
        {
            return false;
        }

        encoder->Prepare(edits.size());
        for (auto& item : edits)
        {
            auto& original = EditRange(item);
            auto range = original;
            if (!encoder->TryEncode(range))
            {
                return false;
            }
            if (range.start.character == original.start.character && range.end.character == original.end.character)
            {
                continue;
            }
            // Stage only changed columns until every target has passed validation.
            if (inlineCount < inlineRanges.size())
            {
                inlineRanges[inlineCount++] = {&original, range};
            }
            else
            {
                overflow.emplace_back(&original, range);
            }
        }
        return true;
    };

    if (edit.changes.has_value())
    {
        for (auto& [uri, edits] : edit.changes.value())
        {
            if (!encodeDocument(uri, edits, nullptr))
            {
                return false;
            }
        }
    }
    if (edit.documentChanges.has_value())
    {
        for (auto& change : edit.documentChanges.value())
        {
            auto* documentEdit = std::get_if<lsp::TextDocumentEdit>(&change);
            if (!documentEdit)
            {
                continue;
            }
            const auto& identifier = documentEdit->textDocument;
            if (!encodeDocument(identifier.uri, documentEdit->edits, &identifier))
            {
                return false;
            }
        }
    }
    for (size_t i = 0; i < inlineCount; ++i)
    {
        const auto& [target, range] = inlineRanges[i];
        *target = range;
    }
    for (const auto& [target, range] : overflow)
    {
        *target = range;
    }
    return true;
}

void Server::EncodeAcrossDocuments(std::vector<lsp::CodeAction>& actions, const OpenDocument& doc) const
{
    ActiveEditDocument active{doc, lsp::Uri::parse(doc.uri), {*doc.text, GetPositionEncoding()}};
    for (auto action = actions.begin(); action != actions.end();)
    {
        if (action->edit && !EncodeWorkspaceEdit(*action->edit, &active))
        {
            action = actions.erase(action);
        }
        else
        {
            ++action;
        }
    }
}
} // namespace angel_lsp
