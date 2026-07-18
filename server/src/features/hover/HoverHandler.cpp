#include "HoverHandler.h"
#include "analysis/SymbolResolver.h"
#include <angelscript.h>
#include <set>

#include <sstream>

namespace angel_lsp
{
namespace features
{

static std::string FormatDoxygen(const std::string& raw, i18n::Locale locale, const std::string& targetParam = "")
{
    if (raw.empty()) return raw;
    std::istringstream iss(raw);
    std::string line;

    std::string brief;
    std::vector<std::string> tparams;
    std::vector<std::string> params;
    std::string returns;
    std::vector<std::string> notes;
    std::vector<std::string> warnings;
    std::vector<std::string> deprecated;

    // Track the active string being appended to (so multiline descriptions work)
    std::string* activeStr = nullptr;
    bool hasTags = false;

    while (std::getline(iss, line))
    {
        size_t firstNonSpace = line.find_first_not_of(" \t\r\n*");
        if (firstNonSpace != std::string::npos && line[firstNonSpace] == '@')
        {
            hasTags = true;
            size_t tagEnd = line.find_first_of(" \t\r\n", firstNonSpace);
            std::string tag = line.substr(firstNonSpace + 1, tagEnd - firstNonSpace - 1);
            std::string content = (tagEnd != std::string::npos) ? line.substr(line.find_first_not_of(" \t\r\n", tagEnd)) : "";

            if (tag == "brief")
            {
                if (!brief.empty()) brief += "\n\n";
                brief += content;
                activeStr = &brief;
            }
            else if (tag == "tparam")
            {
                size_t space = content.find_first_of(" \t");
                if (space != std::string::npos)
                    tparams.push_back("`" + content.substr(0, space) + "` \\- " + content.substr(content.find_first_not_of(" \t", space)));
                else
                    tparams.push_back("`" + content + "`");
                activeStr = &tparams.back();
            }
            else if (tag == "param" || tag.starts_with("param[")) // Handle @param and @param[in]
            {
                std::string inOutStr = "";
                if (tag.starts_with("param["))
                {
                    inOutStr = "[" + tag.substr(6);
                }
                
                size_t space = content.find_first_of(" \t");
                if (space != std::string::npos)
                {
                    std::string pName = "`" + content.substr(0, space) + "`";
                    std::string pDesc = content.substr(content.find_first_not_of(" \t", space));
                    params.push_back(pName + " \\- " + (inOutStr.empty() ? "" : inOutStr + " ") + pDesc);
                }
                else
                {
                    params.push_back("`" + content + "`" + (inOutStr.empty() ? "" : " \\- " + inOutStr));
                }
                activeStr = &params.back();
            }
            else if (tag == "return" || tag == "returns")
            {
                if (!returns.empty()) returns += " ";
                returns += content;
                activeStr = &returns;
            }
            else if (tag == "note")
            {
                notes.push_back(content);
                activeStr = &notes.back();
            }
            else if (tag == "warning")
            {
                warnings.push_back(content);
                activeStr = &warnings.back();
            }
            else if (tag == "deprecated")
            {
                deprecated.push_back(content);
                activeStr = &deprecated.back();
            }
            else
            {
                // Ignore metadata tags like @details, @file, @author, @version, @date, @copyright, @see
                activeStr = nullptr;
                continue;
            }
        }
        else
        {
            if (line.empty()) continue;
            
            // Try to extract pure text if there are no tags
            size_t textStart = line.find_first_not_of(" \t\r\n*/");
            if (textStart == std::string::npos) continue; // Empty line with just stars
            
            std::string pureText = line.substr(textStart);

            if (activeStr)
            {
                *activeStr += " " + pureText;
            }
            else if (!hasTags) // No tags at all? Just dump everything into brief
            {
                if (!brief.empty()) brief += "\n";
                brief += pureText;
            }
        }
    }
    
    std::string out = "";
    if (!brief.empty()) out += brief;

    const auto& s = i18n::GetStrings(locale);

    if (targetParam.empty() && !tparams.empty())
    {
        if (!out.empty()) out += "\n\n";
        out += "**" + std::string(s.hoverTemplateParams) + "**\n\n";
        for (const auto& p : tparams) out += p + "\n\n";
        out.pop_back(); out.pop_back(); // remove trailing \n\n
    }
    
    std::vector<std::string> filteredParams;
    for (const auto& p : params)
    {
        if (targetParam.empty() || p.starts_with("`" + targetParam + "`"))
        {
            filteredParams.push_back(p);
        }
    }

    if (!filteredParams.empty())
    {
        if (!out.empty()) out += "\n\n";
        out += "**" + std::string(s.hoverParams) + "**\n\n";
        for (const auto& p : filteredParams) out += p + "\n\n";
        out.pop_back(); out.pop_back();
    }
    
    if (targetParam.empty())
    {
        if (!returns.empty())
        {
            if (!out.empty()) out += "\n\n";
            out += "**" + std::string(s.hoverReturns) + "**\n\n" + returns;
        }

        for (const auto& note : notes)
        {
            if (!out.empty()) out += "\n\n";
            out += "**" + std::string(s.hoverNote) + "**\n\n" + note;
        }
        for (const auto& warn : warnings)
        {
            if (!out.empty()) out += "\n\n";
            out += "**" + std::string(s.hoverWarning) + "**\n\n" + warn;
        }
        for (const auto& dep : deprecated)
        {
            if (!out.empty()) out += "\n\n";
            out += "**" + std::string(s.hoverDeprecated) + "**\n\n" + dep;
        }
    }

    return out;
}

void ProcessHover(lsp::requests::TextDocument_Hover::Result& result,
                  const lsp::requests::TextDocument_Hover::Params& req,
                  const Document& doc,
                  const analysis::SymbolTable& table,
                  const analysis::DiagnosticCache* diagCache,
                  i18n::Locale locale,
                  const asIScriptEngine* engine)
{
    uint32_t line = req.position.line;
    uint32_t col = req.position.character;
    
    std::string markdown = "";
    const auto& s = i18n::GetStrings(locale);
    
    auto getKindName = [&](analysis::SymbolKind kind)
    {
        switch (kind)
        {
            case analysis::SymbolKind::Variable: return s.kindVariable;
            case analysis::SymbolKind::Function: return s.kindFunction;
            case analysis::SymbolKind::Class: return s.kindClass;
            case analysis::SymbolKind::Namespace: return s.kindNamespace;
            case analysis::SymbolKind::Parameter: return s.kindParameter;
            case analysis::SymbolKind::Property: return s.kindProperty;
            case analysis::SymbolKind::Method: return s.kindMethod;
            case analysis::SymbolKind::Enum: return s.kindEnum;
            case analysis::SymbolKind::EnumMember: return s.kindEnumMember;
            case analysis::SymbolKind::Interface: return s.kindInterface;
            case analysis::SymbolKind::Funcdef: return s.kindFuncdef;
            case analysis::SymbolKind::Mixin: return s.kindMixin;
            case analysis::SymbolKind::Constructor: return s.kindConstructor;
            case analysis::SymbolKind::Destructor: return s.kindDestructor;
            default: return s.kindUnknown;
        }
    };
    
    std::vector<const analysis::Symbol*> multiResults;
    const analysis::Symbol* sym = analysis::SymbolResolver::ResolveAt(doc, table, line, col, &multiResults);
    
    // Dynamic Display Name logic: if we hover over a Class/Enum/Interface/Mixin/Typedef, try to find the full qualified name written by the user.
    // For Parameters/Variables the signature already contains the fully-qualified type (fixed in SymbolCollector), so we don't need this.
    std::string dynamicDisplayName = "";
    if (sym != nullptr && (sym->kind == analysis::SymbolKind::Class || sym->kind == analysis::SymbolKind::Enum || 
                           sym->kind == analysis::SymbolKind::Interface || sym->kind == analysis::SymbolKind::Mixin || 
                           sym->kind == analysis::SymbolKind::Typedef)) 
    {
        TSNode nodeUnder = doc.NodeAt(line, col);
        if (!ts_node_is_null(nodeUnder))
        {
            TSNode current = nodeUnder;
            // Climb up to `type` or similar to find preceding `scope` or `ERROR` nodes
            while (!ts_node_is_null(current))
            {
                std::string_view typeStr = ts_node_type(current);
                if (typeStr == "type" || typeStr == "datatype")
                {
                    TSNode prevSibling = ts_node_prev_sibling(current);
                    if (!ts_node_is_null(prevSibling))
                    {
                        std::string_view prevType = ts_node_type(prevSibling);
                        if (prevType == "scope" || prevType == "ERROR")
                        {
                            std::string_view scopeSv = doc.SourceAt(prevSibling);
                            std::string scopeStr(scopeSv.begin(), scopeSv.end());
                            std::string_view typeSv = doc.SourceAt(current);
                            std::string typeStrText(typeSv.begin(), typeSv.end());
                            // Some AST paths might put the `::` inside the type, some in the scope.
                            if (!scopeStr.empty())
                            {
                                if (typeStrText.starts_with("::"))
                                {
                                    dynamicDisplayName = scopeStr + typeStrText;
                                }
                                else
                                {
                                    dynamicDisplayName = scopeStr + (scopeStr.ends_with("::") ? "" : "::") + typeStrText;
                                }
                            }
                        }
                    }
                    break;
                }
                current = ts_node_parent(current);
            }
        }
    }

    if (sym != nullptr)
    {
        struct GroupedResult
        {
            const analysis::Symbol* sym;
            std::vector<std::string> parents;
        };
        std::vector<GroupedResult> grouped;

        for (const analysis::Symbol* r : multiResults)
        {
            bool foundGroup = false;
            for (auto& g : grouped)
            {
                // To group, they must have same kind, name, and signature/type
                bool sameSignature = r->signature == g.sym->signature;
                bool sameTypeInfo = r->typeInfo == g.sym->typeInfo;
                if (r->kind == g.sym->kind && r->name == g.sym->name && sameSignature && sameTypeInfo)
                {
                    if (r->parent)
                    {
                        // avoid duplicate parent names
                        if (std::find(g.parents.begin(), g.parents.end(), r->parent->name) == g.parents.end())
                        {
                            g.parents.push_back(r->parent->name);
                        }
                    }
                    foundGroup = true;
                    break;
                }
            }
            if (!foundGroup)
            {
                GroupedResult gr;
                gr.sym = r;
                if (r->parent) gr.parents.push_back(r->parent->name);
                grouped.push_back(gr);
            }
        }

        auto renderGrouped = [&](const GroupedResult& gr, const analysis::Symbol* originalSym)
        {
            const analysis::Symbol* renderSym = originalSym ? originalSym : gr.sym;
            // For Parameters/Variables: dispName is always the identifier name (e.g. "target").
            // For types (Class/Enum etc.): dispName can be the full qualified name from the source.
            bool isVarLike = renderSym->kind == analysis::SymbolKind::Parameter ||
                             renderSym->kind == analysis::SymbolKind::Variable  ||
                             renderSym->kind == analysis::SymbolKind::Property;
            std::string dispName = (!isVarLike && !dynamicDisplayName.empty() && renderSym->name == sym->name)
                                        ? dynamicDisplayName
                                        : renderSym->name;
            std::string sig = !renderSym->signature.empty()
                                ? renderSym->signature
                                : (renderSym->typeInfo + (renderSym->typeInfo.empty() ? "" : " ") + dispName);
                                
            if (renderSym->signature.empty())
            {
                if (renderSym->kind == analysis::SymbolKind::Class) sig = "class " + dispName;
                else if (renderSym->kind == analysis::SymbolKind::Interface) sig = "interface " + dispName;
                else if (renderSym->kind == analysis::SymbolKind::Mixin) sig = "mixin " + dispName;
                else if (renderSym->kind == analysis::SymbolKind::Enum) sig = "enum " + dispName;
                else if (renderSym->kind == analysis::SymbolKind::Namespace) sig = "namespace " + dispName;
                else if (renderSym->kind == analysis::SymbolKind::Typedef) sig = "typedef " + renderSym->typeInfo + " " + dispName;
                
                if (renderSym->isShared) sig = "shared " + sig;
            }
            else if (renderSym->kind == analysis::SymbolKind::Parameter && renderSym->parent)
            {
                sig = renderSym->parent->signature;
                if (renderSym->parent->parent && (renderSym->parent->kind == analysis::SymbolKind::Method || renderSym->parent->kind == analysis::SymbolKind::Constructor || renderSym->parent->kind == analysis::SymbolKind::Destructor))
                {
                    size_t paren = sig.find('(');
                    if (paren != std::string::npos)
                    {
                        size_t namePos = sig.rfind(renderSym->parent->name, paren);
                        if (namePos != std::string::npos)
                        {
                            sig.insert(namePos, renderSym->parent->parent->name + "::");
                        }
                    }
                }
            }
            else if (renderSym->parent && (renderSym->kind == analysis::SymbolKind::Method || renderSym->kind == analysis::SymbolKind::Constructor || renderSym->kind == analysis::SymbolKind::Destructor))
            {
                size_t paren = sig.find('(');
                if (paren != std::string::npos)
                {
                    size_t namePos = sig.rfind(renderSym->name, paren);
                    if (namePos != std::string::npos)
                    {
                        sig.insert(namePos, renderSym->parent->name + "::");
                    }
                }
            }
            
            std::string contextStr = "";
            for (size_t i = 0; i < gr.parents.size(); i++)
            {
                if (i > 0) contextStr += ", ";
                contextStr += gr.parents[i];
            }
            
            std::string md = "```angelscript\n" + sig + "\n```\n"
                           + "**" + dispName + "** — " + getKindName(renderSym->kind)
                           + (!contextStr.empty() ? " " + std::string(s.hoverIn) + " `" + contextStr + "`" : "");
            
            std::string docToRender = renderSym->docComment;
            std::string targetParam = "";
            if (docToRender.empty() && renderSym->kind == analysis::SymbolKind::Parameter && renderSym->parent)
            {
                docToRender = renderSym->parent->docComment;
                targetParam = renderSym->name;
            }

            if (!docToRender.empty())
            {
                md += "\n\n---\n" + FormatDoxygen(docToRender, locale, targetParam);
            }
            return md;
        };

        if (grouped.size() > 1 || (!grouped.empty() && grouped[0].parents.size() > 1))
        {
            for (size_t i = 0; i < grouped.size(); i++)
            {
                if (i > 0) markdown += "\n\n---\n\n";
                markdown += renderGrouped(grouped[i], nullptr);
            }
        }
        else
        {
            GroupedResult gr;
            gr.sym = sym;
            if (sym->parent) gr.parents.push_back(sym->parent->name);
            markdown = renderGrouped(gr, sym);
        }
    }
    else
    {
        TSNode nodeUnder = doc.NodeAt(line, col);
        if (!ts_node_is_null(nodeUnder))
        {
            std::string_view sv = doc.SourceAt(nodeUnder);
            std::string name(sv.begin(), sv.end());
            
            if (engine)
            {
                for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++)
                {
                    asIScriptFunction* func = engine->GetGlobalFunctionByIndex(i);
                    if (func && std::string(func->GetName()) == name)
                    {
                        std::string decl = func->GetDeclaration(true, true, true);
                        markdown = "```angelscript\n" + decl + "\n```\n**" + name + "** — " + s.hoverBuiltinFunc;
                        break;
                    }
                }
                if (markdown.empty())
                {
                    int typeId = engine->GetTypeIdByDecl(name.c_str());
                    if (typeId >= 0)
                    {
                        asITypeInfo* type = engine->GetTypeInfoById(typeId);
                        if (type)
                        {
                            markdown = "```angelscript\nclass " + std::string(type->GetName()) + "\n```\n**" + name + "** — " + s.hoverBuiltinType;
                        }
                    }
                }
            }
        }
    }

    // Append Engine Diagnostics
    if (diagCache)
    {
        auto diags = diagCache->GetAt(req.textDocument.uri.toString(), line, col);
        for (const auto* d : diags)
        {
            if (!markdown.empty()) markdown += "\n\n---\n\n";
            std::string prefix = d->severity == lsp::DiagnosticSeverity::Error ? "⛔ **" + std::string(s.hoverEngineError) + "**" : "⚠️ **" + std::string(s.hoverEngineWarn) + "**";
            markdown += prefix + " `" + d->message + "`";
        }
    }

    if (markdown.empty()) return;

    result = lsp::Hover{};
    auto& hover = *result;
    lsp::MarkupContent markup;
    markup.kind = lsp::MarkupKind::Markdown;
    markup.value = markdown;
    hover.contents = markup;
}

}
}
