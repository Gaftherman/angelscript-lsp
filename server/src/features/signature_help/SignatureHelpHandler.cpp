/**
 * @file SignatureHelpHandler.cpp
 * @brief Implementation of textDocument/signatureHelp request processor.
 * @ingroup Features
 */

#include "SignatureHelpHandler.h"
#include <optional>

namespace angel_lsp::features::signature_help
{
    struct CallContext
    {
        std::string funcName;
        int activeParameter;
    };

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

    static std::optional<CallContext> FindCallContext(const std::string &text, size_t offset)
    {
        size_t pos = offset;
        int commas = 0;
        int depth = 0;

        while (pos > 0)
        {
            pos--;
            char c = text[pos];

            if (c == '"')
            {
                while (pos > 0)
                {
                    pos--;
                    if (text[pos] == '"' && text[pos - 1] != '\\')
                    {
                        break;
                    }
                }
                continue;
            }

            if (c == ')')
            {
                depth++;
            }
            else if (c == '(')
            {
                if (depth > 0)
                {
                    depth--;
                }
                else
                {
                    std::string funcName = ReadIdentifierBefore(text, pos);
                    if (!funcName.empty())
                    {
                        return CallContext{funcName, commas};
                    }
                    return std::nullopt;
                }
            }
            else if (c == ',' && depth == 0)
            {
                commas++;
            }
        }
        return std::nullopt;
    }

    lsp::requests::TextDocument_SignatureHelp::Result ProcessSignatureHelp(
        const lsp::requests::TextDocument_SignatureHelp::Params &req,
        const Document &doc,
        const analysis::SymbolTable &table)
    {
        lsp::requests::TextDocument_SignatureHelp::Result res;

        size_t offset = GetByteOffset(doc.GetText(), req.position.line, req.position.character);
        auto callCtx = FindCallContext(doc.GetText(), offset);
        if (!callCtx)
        {
            return res;
        }

        res = lsp::SignatureHelp{};
        auto &help = *res;
        help.signatures = {};

        auto overloads = table.FindByName(callCtx->funcName);
        for (const auto *sym : overloads)
        {
            if (sym->kind == analysis::SymbolKind::Function || sym->kind == analysis::SymbolKind::Method)
            {
                lsp::SignatureInformation sig;
                sig.label = !sym->BuildSignature().empty() ? sym->BuildSignature() : (sym->typeInfo + (sym->typeInfo.empty() ? "" : " ") + sym->name + "()");

                sig.parameters = std::vector<lsp::ParameterInformation>{};
                for (const auto &p : sym->params)
                {
                    lsp::ParameterInformation pInfo;
                    pInfo.label = p.typeName + (p.name.empty() ? "" : " " + p.name);
                    sig.parameters->push_back(pInfo);
                }

                help.signatures.push_back(sig);
            }
        }

        if (!help.signatures.empty())
        {
            help.activeSignature = 0;
            help.activeParameter = callCtx->activeParameter;
        }

        return res;
    }

} // namespace angel_lsp::features::signature_help
