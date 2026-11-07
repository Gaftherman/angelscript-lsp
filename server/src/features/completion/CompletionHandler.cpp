#include "features/completion/CompletionHandler.h"

#include <utility>

namespace angel_lsp::features
{

/**
 * @brief Maps an engine entry kind to the protocol completion kind.
 * @param[in] kind Kind chosen by the completion engine.
 * @return The LSP completion item kind with the same meaning.
 */
static lsp::CompletionItemKind ToLspKind(analysis::completion::CompletionEntryKind kind)
{
    switch (kind)
    {
    case analysis::completion::CompletionEntryKind::Method:
        return lsp::CompletionItemKind::Method;
    case analysis::completion::CompletionEntryKind::Function:
        return lsp::CompletionItemKind::Function;
    case analysis::completion::CompletionEntryKind::Field:
        return lsp::CompletionItemKind::Field;
    case analysis::completion::CompletionEntryKind::Class:
        return lsp::CompletionItemKind::Class;
    case analysis::completion::CompletionEntryKind::Interface:
        return lsp::CompletionItemKind::Interface;
    case analysis::completion::CompletionEntryKind::Module:
        return lsp::CompletionItemKind::Module;
    case analysis::completion::CompletionEntryKind::Property:
        return lsp::CompletionItemKind::Property;
    case analysis::completion::CompletionEntryKind::Enum:
        return lsp::CompletionItemKind::Enum;
    case analysis::completion::CompletionEntryKind::Keyword:
        return lsp::CompletionItemKind::Keyword;
    case analysis::completion::CompletionEntryKind::Snippet:
        return lsp::CompletionItemKind::Snippet;
    case analysis::completion::CompletionEntryKind::File:
        return lsp::CompletionItemKind::File;
    case analysis::completion::CompletionEntryKind::EnumMember:
        return lsp::CompletionItemKind::EnumMember;
    case analysis::completion::CompletionEntryKind::TypeParameter:
        return lsp::CompletionItemKind::TypeParameter;
    case analysis::completion::CompletionEntryKind::Variable:
        break;
    }
    return lsp::CompletionItemKind::Variable;
}

/**
 * @brief Converts one engine entry into an LSP completion item, moving its text fields.
 * @param[in,out] entry Entry produced by the completion engine; its strings are moved out.
 * @return Equivalent LSP item: the kind is mapped with ToLspKind, a snippet
 *         entry gets the snippet insert format, documentation becomes markdown, and the resolve key is
 *         stored in `data` for ResolveCompletionItem.
 */
static lsp::CompletionItem ToLspCompletionItem(analysis::completion::CompletionEntry&& entry)
{
    lsp::CompletionItem item;
    item.label = std::move(entry.label);
    if (entry.kind)
    {
        item.kind = ToLspKind(*entry.kind);
    }
    item.detail = std::move(entry.detail);
    item.insertText = std::move(entry.insertText);
    if (entry.isSnippet)
    {
        item.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
    }
    item.sortText = std::move(entry.sortText);
    if (entry.documentation)
    {
        item.documentation =
            lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(*entry.documentation)};
    }
    if (entry.resolveKey)
    {
        item.data = lsp::LSPAny(std::move(*entry.resolveKey));
    }
    if (entry.textEdit)
    {
        lsp::TextEdit edit;
        edit.range.start.line = entry.textEdit->line;
        edit.range.start.character = entry.textEdit->startCharacter;
        edit.range.end.line = entry.textEdit->line;
        edit.range.end.character = entry.textEdit->endCharacter;
        edit.newText = std::move(entry.textEdit->newText);
        item.textEdit = std::move(edit);
    }
    return item;
}

std::vector<lsp::CompletionItem> GetCompletion(const analysis::completion::CompletionQuery& query)
{
    auto entries = analysis::completion::CollectCompletionEntries(query);
    std::vector<lsp::CompletionItem> items;
    items.reserve(entries.size());
    for (auto& entry : entries)
    {
        items.push_back(ToLspCompletionItem(std::move(entry)));
    }
    return items;
}

lsp::CompletionItem ResolveCompletionItem(const CompletionResolveRequest& request)
{
    lsp::CompletionItem resolved = request.item;
    if (resolved.documentation.has_value() || !resolved.data.has_value() || !resolved.data->isString())
    {
        return resolved;
    }
    if (auto documentation = analysis::completion::ResolveCompletionDocumentation(
            resolved.data->string(), request.symbolTable, request.readDocument))
    {
        resolved.documentation = lsp::MarkupContent{
            .kind = lsp::MarkupKind::Markdown,
            .value = std::move(*documentation),
        };
    }
    return resolved;
}

} // namespace angel_lsp::features
