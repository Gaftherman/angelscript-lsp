#include "HoverHandler.h"
#include "analysis/SymbolResolver.h"
#include <angelscript.h>
#include <set>

namespace angel_lsp {
namespace features {



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
    
    auto getKindName = [&](analysis::SymbolKind kind) {
        switch (kind) {
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
        if (!ts_node_is_null(nodeUnder)) {
            TSNode current = nodeUnder;
            // Climb up to `type` or similar to find preceding `scope` or `ERROR` nodes
            while (!ts_node_is_null(current)) {
                std::string_view typeStr = ts_node_type(current);
                if (typeStr == "type" || typeStr == "datatype") {
                    TSNode prevSibling = ts_node_prev_sibling(current);
                    if (!ts_node_is_null(prevSibling)) {
                        std::string_view prevType = ts_node_type(prevSibling);
                        if (prevType == "scope" || prevType == "ERROR") {
                            std::string_view scopeSv = doc.SourceAt(prevSibling);
                            std::string scopeStr(scopeSv.begin(), scopeSv.end());
                            std::string_view typeSv = doc.SourceAt(current);
                            std::string typeStrText(typeSv.begin(), typeSv.end());
                            // Some AST paths might put the `::` inside the type, some in the scope.
                            if (!scopeStr.empty()) {
                                if (typeStrText.starts_with("::")) {
                                    dynamicDisplayName = scopeStr + typeStrText;
                                } else {
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
        struct GroupedResult {
            const analysis::Symbol* sym;
            std::vector<std::string> parents;
        };
        std::vector<GroupedResult> grouped;

        for (const analysis::Symbol* r : multiResults) {
            bool foundGroup = false;
            for (auto& g : grouped) {
                // To group, they must have same kind, name, and signature/type
                bool sameSignature = r->signature == g.sym->signature;
                bool sameTypeInfo = r->typeInfo == g.sym->typeInfo;
                if (r->kind == g.sym->kind && r->name == g.sym->name && sameSignature && sameTypeInfo) {
                    if (r->parent) {
                        // avoid duplicate parent names
                        if (std::find(g.parents.begin(), g.parents.end(), r->parent->name) == g.parents.end()) {
                            g.parents.push_back(r->parent->name);
                        }
                    }
                    foundGroup = true;
                    break;
                }
            }
            if (!foundGroup) {
                GroupedResult gr;
                gr.sym = r;
                if (r->parent) gr.parents.push_back(r->parent->name);
                grouped.push_back(gr);
            }
        }

        auto renderGrouped = [&](const GroupedResult& gr, const analysis::Symbol* originalSym) {
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
            
            std::string contextStr = "";
            for (size_t i = 0; i < gr.parents.size(); i++) {
                if (i > 0) contextStr += ", ";
                contextStr += gr.parents[i];
            }
            
            std::string md = "```angelscript\n" + sig + "\n```\n"
                           + "**" + dispName + "** — " + getKindName(renderSym->kind)
                           + (!contextStr.empty() ? " " + std::string(s.hoverIn) + " `" + contextStr + "`" : "");
            
            if (!renderSym->docComment.empty()) {
                md += "\n\n" + renderSym->docComment;
            }
            return md;
        };

        if (grouped.size() > 1 || (!grouped.empty() && grouped[0].parents.size() > 1)) {
            for (size_t i = 0; i < grouped.size(); i++) {
                if (i > 0) markdown += "\n\n---\n\n";
                markdown += renderGrouped(grouped[i], nullptr);
            }
        } else {
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
            
            if (engine) {
                for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++) {
                    asIScriptFunction* func = engine->GetGlobalFunctionByIndex(i);
                    if (func && std::string(func->GetName()) == name) {
                        std::string decl = func->GetDeclaration(true, true, true);
                        markdown = "```angelscript\n" + decl + "\n```\n**" + name + "** — " + s.hoverBuiltinFunc;
                        break;
                    }
                }
                if (markdown.empty()) {
                    int typeId = engine->GetTypeIdByDecl(name.c_str());
                    if (typeId >= 0) {
                        asITypeInfo* type = engine->GetTypeInfoById(typeId);
                        if (type) {
                            markdown = "```angelscript\nclass " + std::string(type->GetName()) + "\n```\n**" + name + "** — " + s.hoverBuiltinType;
                        }
                    }
                }
            }
        }
    }

    // Append Engine Diagnostics
    if (diagCache) {
        auto diags = diagCache->GetAt(req.textDocument.uri.toString(), line, col);
        for (const auto* d : diags) {
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
