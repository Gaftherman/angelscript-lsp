/**
 * @file HoverHandler.cpp
 * @brief Implementation of textDocument/hover request processor.
 * @ingroup Features
 */

#include "HoverHandler.h"
#include "features/hover/HoverInfo.h"
#include "utils/DoxygenParser.h"
#include "analysis/SymbolResolver.h"
#include "utils/LspLogger.h"
#include <ankerl/unordered_dense.h>
#include <set>
#include <unordered_map>
#include <mutex>
#include <sstream>

namespace angel_lsp::features::hover
{
    static std::string BuildFullNamespace(const analysis::Symbol *nsSym)
    {
        std::string ns = nsSym->name;
        const analysis::Symbol *curr = nsSym->parent;
        while (curr && curr->kind == analysis::SymbolKind::Namespace)
        {
            ns = curr->name + "::" + ns;
            curr = curr->parent;
        }
        return ns;
    }

    static std::string BuildScopeContext(const analysis::Symbol *sym)
    {
        if (!sym || !sym->parent)
        {
            return "";
        }

        if (sym->parent->kind == analysis::SymbolKind::Class ||
            sym->parent->kind == analysis::SymbolKind::Interface ||
            sym->parent->kind == analysis::SymbolKind::Mixin)
        {
            std::string scope = sym->parent->name;
            if (!sym->parent->templateParam.empty())
            {
                scope += "<" + sym->parent->templateParam + ">";
            }
            if (sym->parent->parent && sym->parent->parent->kind == analysis::SymbolKind::Namespace)
            {
                scope = BuildFullNamespace(sym->parent->parent) + "::" + scope;
            }
            return scope;
        }

        if (sym->parent->kind == analysis::SymbolKind::Enum)
        {
            std::string scope = sym->parent->name;
            if (sym->parent->parent)
            {
                if (sym->parent->parent->kind == analysis::SymbolKind::Namespace)
                {
                    scope = BuildFullNamespace(sym->parent->parent) + "::" + scope;
                }
                else if (sym->parent->parent->kind == analysis::SymbolKind::Class ||
                         sym->parent->parent->kind == analysis::SymbolKind::Interface)
                {
                    std::string parentScope = BuildScopeContext(sym->parent);
                    if (!parentScope.empty())
                    {
                        scope = parentScope + "::" + scope;
                    }
                }
            }
            return scope;
        }

        if (sym->parent->kind == analysis::SymbolKind::Namespace)
        {
            return BuildFullNamespace(sym->parent);
        }

        return "";
    }

    static std::string ExtractBriefDocumentation(const std::string &docComment)
    {
        if (docComment.empty())
        {
            return "";
        }

        utils::DoxygenDoc doc = utils::ParseDoxygen(docComment);

        if (!doc.brief.empty())
        {
            return doc.brief;
        }

        if (!doc.details.empty())
        {
            size_t periodPos = doc.details.find('.');
            if (periodPos != std::string::npos)
            {
                return doc.details.substr(0, periodPos + 1);
            }
            return doc.details;
        }

        return "";
    }

    static std::string BuildOverloadDescription(const analysis::Symbol *overloadSym)
    {
        if (!overloadSym)
        {
            return "";
        }

        std::string sig = overloadSym->BuildSignature();
        std::string docBrief = ExtractBriefDocumentation(overloadSym->docComment);

        if (!docBrief.empty())
        {
            return "`" + sig + "`  \n*" + docBrief + "*";
        }
        return "`" + sig + "`";
    }

    static HoverInfo BuildHoverInfo(
        const analysis::Symbol *primarySym,
        const analysis::Symbol *resolvedSym,
        const std::vector<const analysis::Symbol *> &allOverloads,
        const Document &doc,
        const analysis::SymbolTable &table,
        const i18n::LspStrings &s,
        i18n::Locale locale,
        const std::string &dynamicDisplayName = "",
        const std::string &templateSub = "")
    {
        HoverInfo info;

        if (!primarySym)
        {
            return info;
        }

        info.name = dynamicDisplayName.empty() ? primarySym->name : dynamicDisplayName;
        info.kind = primarySym->kind;
        info.briefText = ExtractBriefDocumentation(primarySym->docComment);

        std::string scope = BuildScopeContext(primarySym);
        info.localScope = scope;

        if (primarySym->kind == analysis::SymbolKind::Function ||
            primarySym->kind == analysis::SymbolKind::Method)
        {
            std::string sig = primarySym->BuildSignature();
            if (!templateSub.empty() && !primarySym->templateParam.empty())
            {
                size_t pos = 0;
                while ((pos = sig.find(primarySym->templateParam, pos)) != std::string::npos)
                {
                    sig.replace(pos, primarySym->templateParam.length(), templateSub);
                    pos += templateSub.length();
                }
            }

            if (!scope.empty())
            {
                sig = scope + "::" + sig;
            }
            info.rawSignature = sig;

            if (allOverloads.size() > 1)
            {
                info.overloadCount = static_cast<int>(allOverloads.size());
            }
        }
        else if (primarySym->kind == analysis::SymbolKind::Variable ||
                 primarySym->kind == analysis::SymbolKind::Property ||
                 primarySym->kind == analysis::SymbolKind::Parameter)
        {
            std::string sig = primarySym->BuildSignature();
            if (!scope.empty())
            {
                sig = scope + "::" + sig;
            }
            info.rawSignature = sig;

            if (!primarySym->value.empty())
            {
                info.enumValue = primarySym->value;
            }
        }
        else if (primarySym->kind == analysis::SymbolKind::Class ||
                 primarySym->kind == analysis::SymbolKind::Interface ||
                 primarySym->kind == analysis::SymbolKind::Mixin)
        {
            std::string prefix;
            if (primarySym->kind == analysis::SymbolKind::Interface)
            {
                prefix = "interface ";
            }
            else if (primarySym->kind == analysis::SymbolKind::Mixin)
            {
                prefix = "mixin class ";
            }
            else
            {
                prefix = "class ";
            }

            std::string sig = prefix + primarySym->name;
            if (!primarySym->templateParam.empty())
            {
                sig += "<" + primarySym->templateParam + ">";
            }

            if (!primarySym->baseClasses.empty())
            {
                sig += " : ";
                for (size_t i = 0; i < primarySym->baseClasses.size(); ++i)
                {
                    if (i > 0)
                    {
                        sig += ", ";
                    }
                    sig += primarySym->baseClasses[i];
                }
            }

            if (!scope.empty())
            {
                sig = scope + "::" + sig;
            }
            info.rawSignature = sig;
        }
        else if (primarySym->kind == analysis::SymbolKind::Enum)
        {
            std::string sig = "enum " + primarySym->name;
            if (!scope.empty())
            {
                sig = scope + "::" + sig;
            }
            info.rawSignature = sig;
        }
        else if (primarySym->kind == analysis::SymbolKind::EnumMember)
        {
            std::string sig = primarySym->name;
            if (!primarySym->value.empty())
            {
                sig += " = " + primarySym->value;
            }
            if (!scope.empty())
            {
                sig = scope + "::" + sig;
            }
            info.rawSignature = sig;
            info.enumValue = primarySym->value;
        }
        else if (primarySym->kind == analysis::SymbolKind::Typedef)
        {
            std::string sig = "typedef " + primarySym->typeInfo + " " + primarySym->name;
            if (!scope.empty())
            {
                sig = scope + "::" + sig;
            }
            info.rawSignature = sig;
        }
        else if (primarySym->kind == analysis::SymbolKind::Funcdef)
        {
            std::string sig = "funcdef " + primarySym->BuildSignature();
            if (!scope.empty())
            {
                sig = scope + "::" + sig;
            }
            info.rawSignature = sig;
        }
        else
        {
            info.rawSignature = primarySym->BuildSignature();
        }

        return info;
    }

    void ProcessHover(lsp::requests::TextDocument_Hover::Result &result,
                      const lsp::requests::TextDocument_Hover::Params &req,
                      const Document &doc,
                      const analysis::SymbolTable &table,
                      const analysis::DiagnosticCache *diagCache,
                      i18n::Locale locale)
    {
        result = nullptr;

        const auto &s = i18n::GetStrings(locale);

        uint32_t line = req.position.line;
        uint32_t col = req.position.character;

        std::vector<const analysis::Symbol *> multiResults;
        const analysis::Symbol *sym = analysis::SymbolResolver::ResolveAt(doc, table, line, col, &multiResults);

        std::vector<HoverInfo::HoverSection> hoverSections;

        std::string dynamicDisplayName;
        std::string templateSubstitution;

        if (sym != nullptr && sym->kind == analysis::SymbolKind::Method)
        {
            TSNode nodeUnder = doc.NodeAt(line, col);
            if (!ts_node_is_null(nodeUnder))
            {
                TSNode parent = ts_node_parent(nodeUnder);
                std::string_view parentType = ts_node_is_null(parent) ? "" : ts_node_type(parent);
                if (parentType == "member_expression" || parentType == "field_access")
                {
                    TSNode memberNode = ts_node_child_by_field_name(parent, "member", sizeof("member") - 1);
                    if (ts_node_is_null(memberNode) || !ts_node_eq(nodeUnder, memberNode))
                    {
                        memberNode = ts_node_child_by_field_name(parent, "field", sizeof("field") - 1);
                    }

                    if (!ts_node_is_null(memberNode) && ts_node_eq(nodeUnder, memberNode))
                    {
                        TSNode objectNode = ts_node_child_by_field_name(parent, "object", sizeof("object") - 1);
                        if (!ts_node_is_null(objectNode))
                        {
                            std::string_view objSv = doc.SourceAt(objectNode);
                            std::string objText(objSv.begin(), objSv.end());
                            const analysis::Symbol *objSym = table.FindLocalByName(objText);
                            if (!objSym)
                            {
                                objSym = table.FindGlobalByName(objText);
                            }

                            if (objSym && !objSym->typeInfo.empty())
                            {
                                size_t openT = objSym->typeInfo.find('<');
                                size_t closeT = objSym->typeInfo.rfind('>');
                                if (openT != std::string::npos && closeT != std::string::npos && closeT > openT)
                                {
                                    templateSubstitution = objSym->typeInfo.substr(openT + 1, closeT - openT - 1);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (sym != nullptr)
        {
            HoverInfo info = BuildHoverInfo(sym, sym, multiResults, doc, table, s, locale, dynamicDisplayName, templateSubstitution);
            hoverSections = info.ToHoverSections(locale);
        }

        if (diagCache)
        {
            auto diags = diagCache->GetAt(req.textDocument.uri.toString(), line, col);
            for (const auto *d : diags)
            {
                HoverInfo::HoverSection diagSection;
                diagSection.isCodeBlock = false;
                std::string prefix = d->severity == lsp::DiagnosticSeverity::Error ? "**" + std::string(s.hoverEngineError) + "**" : "**" + std::string(s.hoverEngineWarn) + "**";
                diagSection.content = prefix + " `" + d->message + "`";
                hoverSections.push_back(diagSection);
            }
        }

        if (hoverSections.empty())
        {
            return;
        }

        result = lsp::Hover{};
        auto &hover = *result;

        lsp::Array<lsp::MarkedString> markedStrings;
        for (const auto &section : hoverSections)
        {
            if (section.isCodeBlock)
            {
                lsp::MarkedString_Language_Value ms;
                ms.language = section.language;
                ms.value = section.content;
                markedStrings.push_back(ms);
            }
            else
            {
                markedStrings.push_back(lsp::String{section.content});
            }
        }
        hover.contents = markedStrings;
    }
} // namespace angel_lsp::features::hover
