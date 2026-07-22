/**
 * @file CompletionHandler.cpp
 * @brief Implementation of textDocument/completion request processor.
 * @ingroup Features
 */

#include "CompletionHandler.h"
#include "analysis/SymbolTable.h"

namespace angel_lsp::features::completion
{
    /**
     * @brief Type of auto-completion context at cursor location.
     */
    enum class CompletionContextType
    {
        Global,    /**< Global scope or local variable scope. */
        Member,    /**< Member access trigger (e.g. `object.`). */
        Namespace  /**< Namespace scope resolution trigger (e.g. `Namespace::`). */
    };

    /**
     * @brief Holds analyzed completion context state.
     */
    struct CompletionContext
    {
        CompletionContextType type; /**< Type of completion context. */
        std::string scopeName;     /**< Container/object name for member or namespace completion. */
    };

    /**
     * @brief Computes 0-based byte offset from line and column position.
     *
     * @param[in] text The document text string.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @return size_t Byte offset into text.
     */
    static size_t GetByteOffset(const std::string &text, uint32_t line, uint32_t col)
    {
        size_t offset = 0;
        uint32_t currentLine = 0;
        while (currentLine < line && offset < text.length())
        {
            if (text[offset] == '\n')
            {
                currentLine++;
            }
            offset++;
        }
        return std::min(offset + col, text.length());
    }

    /**
     * @brief Reads backwards from position to extract previous identifier token.
     *
     * @param[in] text The document text string.
     * @param[in] pos Starting byte offset.
     * @return std::string Extracted identifier string.
     */
    static std::string ReadIdentifierBefore(const std::string &text, size_t pos)
    {
        while (pos > 0 && std::isspace(text[pos - 1]))
        {
            pos--;
        }
        size_t end = pos;
        while (pos > 0 && (std::isalnum(text[pos - 1]) || text[pos - 1] == '_'))
        {
            pos--;
        }
        return text.substr(pos, end - pos);
    }

    /**
     * @brief Analyzes code context around cursor position to determine completion trigger mode.
     *
     * @param[in] text The document text string.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @return CompletionContext Analyzed completion context.
     */
    static CompletionContext AnalyzeContext(const std::string &text, uint32_t line, uint32_t col)
    {
        size_t pos = GetByteOffset(text, line, col);

        while (pos > 0 && std::isspace(text[pos - 1]))
        {
            pos--;
        }

        if (pos > 0 && text[pos - 1] == '.')
        {
            std::string obj = ReadIdentifierBefore(text, pos - 1);
            if (!obj.empty())
            {
                return {CompletionContextType::Member, obj};
            }
        }
        else if (pos > 1 && text[pos - 2] == ':' && text[pos - 1] == ':')
        {
            std::string ns = ReadIdentifierBefore(text, pos - 2);
            if (!ns.empty())
            {
                return {CompletionContextType::Namespace, ns};
            }
        }

        return {CompletionContextType::Global, ""};
    }

    static lsp::CompletionItemKind SymbolKindToCompletionKind(analysis::SymbolKind kind)
    {
        switch (kind)
        {
        case analysis::SymbolKind::Class:
            return lsp::CompletionItemKind::Class;
        case analysis::SymbolKind::Method:
            return lsp::CompletionItemKind::Method;
        case analysis::SymbolKind::Property:
            return lsp::CompletionItemKind::Field;
        case analysis::SymbolKind::Function:
            return lsp::CompletionItemKind::Function;
        case analysis::SymbolKind::Variable:
            return lsp::CompletionItemKind::Variable;
        case analysis::SymbolKind::Parameter:
            return lsp::CompletionItemKind::Variable;
        case analysis::SymbolKind::Namespace:
            return lsp::CompletionItemKind::Module;
        case analysis::SymbolKind::Enum:
            return lsp::CompletionItemKind::Enum;
        case analysis::SymbolKind::EnumMember:
            return lsp::CompletionItemKind::EnumMember;
        case analysis::SymbolKind::Interface:
            return lsp::CompletionItemKind::Interface;
        case analysis::SymbolKind::Funcdef:
            return lsp::CompletionItemKind::Function;
        case analysis::SymbolKind::Mixin:
            return lsp::CompletionItemKind::Class;
        case analysis::SymbolKind::Typedef:
            return lsp::CompletionItemKind::Struct;
        default:
            return lsp::CompletionItemKind::Text;
        }
    }

    lsp::requests::TextDocument_Completion::Result ProcessCompletion(
        const lsp::requests::TextDocument_Completion::Params &req,
        const Document &doc,
        const analysis::SymbolTable &table)
    {
        lsp::requests::TextDocument_Completion::Result res;
        std::vector<lsp::CompletionItem> items;

        CompletionContext ctx = AnalyzeContext(doc.GetText(), req.position.line, req.position.character);

        if (ctx.type == CompletionContextType::Global)
        {
            for (const auto &[name, syms] : table.GetGlobals())
            {
                for (const auto &sym : syms)
                {
                    lsp::CompletionItem item;
                    item.label = sym->name;
                    item.kind = SymbolKindToCompletionKind(sym->kind);
                    item.detail = !sym->BuildSignature().empty() ? sym->BuildSignature() : (sym->typeInfo + (sym->typeInfo.empty() ? "" : " ") + sym->name);
                    items.push_back(item);
                }
            }

            const char *keywords[] = {
                "and", "abstract", "auto", "bool", "break", "case", "cast", "class", "const", "continue", "default", "do", "double", "else", "enum", "explicit", "external", "false", "final", "float", "for", "from", "funcdef", "get", "if", "import", "in", "inout", "int", "int8", "int16", "int32", "int64", "interface", "is", "mixin", "namespace", "not", "null", "or", "out", "override", "private", "property", "protected", "return", "set", "shared", "stat", "switch", "this", "true", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "void", "while", "xor"};
            for (const char *kw : keywords)
            {
                lsp::CompletionItem item;
                item.label = kw;
                item.kind = lsp::CompletionItemKind::Keyword;
                items.push_back(item);
            }
        }
        else if (ctx.type == CompletionContextType::Member)
        {
            const analysis::Symbol *objectSym = table.FindFirst(ctx.scopeName);
            if (objectSym)
            {
                std::string typeName = objectSym->typeInfo;
                if (!typeName.empty() && typeName.back() == '@')
                {
                    typeName.pop_back();
                }

                auto members = table.FindInContainer(typeName);
                for (const auto &sym : members)
                {
                    lsp::CompletionItem item;
                    item.label = sym->name;
                    item.kind = SymbolKindToCompletionKind(sym->kind);
                    item.detail = !sym->BuildSignature().empty() ? sym->BuildSignature() : (sym->typeInfo + (sym->typeInfo.empty() ? "" : " ") + sym->name);
                    items.push_back(item);
                }
            }
        }
        else if (ctx.type == CompletionContextType::Namespace)
        {
            auto members = table.FindInContainer(ctx.scopeName);
            for (const auto &sym : members)
            {
                lsp::CompletionItem item;
                item.label = sym->name;
                item.kind = SymbolKindToCompletionKind(sym->kind);
                item.detail = !sym->BuildSignature().empty() ? sym->BuildSignature() : (sym->typeInfo + (sym->typeInfo.empty() ? "" : " ") + sym->name);
                items.push_back(item);
            }
        }

        res = items;
        return res;
    }

} // namespace angel_lsp::features::completion
