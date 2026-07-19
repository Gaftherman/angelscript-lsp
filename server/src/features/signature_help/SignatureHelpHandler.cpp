#include "SignatureHelpHandler.h"
#include <angelscript.h>
#include <optional>

namespace angel_lsp
{
    namespace features
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
                pos--;
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
                        if (text[pos] == '"')
                        {
                            if (pos == 0 || text[pos - 1] != '\\')
                                break;
                        }
                    }
                    continue;
                }

                if (c == ')' || c == ']')
                {
                    depth++;
                    continue;
                }

                if (c == '(' || c == '[')
                {
                    if (depth > 0)
                    {
                        depth--;
                        continue;
                    }
                    else
                    {
                        if (c == '(')
                        {
                            std::string funcName = ReadIdentifierBefore(text, pos);
                            if (!funcName.empty())
                            {
                                return CallContext{funcName, commas};
                            }
                        }
                        return std::nullopt;
                    }
                }

                if (c == ',' && depth == 0)
                {
                    commas++;
                }

                if (c == '{' || c == '}' || c == ';')
                {
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        lsp::requests::TextDocument_SignatureHelp::Result ProcessSignatureHelp(
            const lsp::requests::TextDocument_SignatureHelp::Params &req,
            const Document &doc,
            const analysis::SymbolTable &table,
            const asIScriptEngine *engine)
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
                    help.signatures.push_back(sig);
                }
            }

            if (engine)
            {
                for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++)
                {
                    asIScriptFunction *func = engine->GetGlobalFunctionByIndex(i);
                    if (func && std::string(func->GetName()) == callCtx->funcName)
                    {
                        lsp::SignatureInformation sig;
                        sig.label = func->GetDeclaration(false, true, true);
                        sig.parameters = std::vector<lsp::ParameterInformation>{};

                        for (asUINT p = 0; p < func->GetParamCount(); p++)
                        {
                            int typeId;
                            asDWORD flags;
                            const char *name = nullptr;
                            func->GetParam(p, &typeId, &flags, &name);

                            lsp::ParameterInformation paramInfo;
                            paramInfo.label = name ? name : "";
                            sig.parameters->push_back(paramInfo);
                        }

                        help.signatures.push_back(sig);
                    }
                }
            }

            if (!help.signatures.empty())
            {
                help.activeSignature = 0;
                help.activeParameter = callCtx->activeParameter;
            }

            return res;
        }

    }
}
