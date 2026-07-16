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
    
    // Dynamic Display Name logic: if we hover over a Class/Enum/Interface, try to find the full qualified name written by the user.
    std::string dynamicDisplayName = "";
    if (sym != nullptr && (sym->kind == analysis::SymbolKind::Class || sym->kind == analysis::SymbolKind::Enum || 
                           sym->kind == analysis::SymbolKind::Interface || sym->kind == analysis::SymbolKind::Mixin || 
                           sym->kind == analysis::SymbolKind::Typedef || sym->kind == analysis::SymbolKind::Parameter ||
                           sym->kind == analysis::SymbolKind::Variable)) 
    {
        TSNode nodeUnder = doc.NodeAt(line, col);
        if (!ts_node_is_null(nodeUnder)) {
            TSNode current = nodeUnder;
            
            if (sym->kind == analysis::SymbolKind::Parameter || sym->kind == analysis::SymbolKind::Variable) {
                // Climb up to `parameter` or `variable_declaration`
                while (!ts_node_is_null(current)) {
                    std::string_view typeStr = ts_node_type(current);
                    if (typeStr == "parameter" || typeStr == "declaration") {
                        TSNode prevSibling = ts_node_prev_sibling(current);
                        if (!ts_node_is_null(prevSibling) && std::string_view(ts_node_type(prevSibling)) == "ERROR") {
                            std::string_view errSv = doc.SourceAt(prevSibling);
                            std::string errStr(errSv.begin(), errSv.end());
                            if (!errStr.empty()) {
                                if (sym->typeInfo.starts_with("::")) {
                                    dynamicDisplayName = errStr + sym->typeInfo;
                                } else {
                                    dynamicDisplayName = errStr + "::" + sym->typeInfo;
                                }
                            }
                        }
                        break;
                    }
                    current = ts_node_parent(current);
                }
            } else {
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
    }

    if (sym != nullptr)
    {
        bool multipleDistinctKinds = false;
        bool multipleDistinctParents = false;
        if (multiResults.size() > 1) {
            analysis::SymbolKind firstKind = multiResults[0]->kind;
            const analysis::Symbol* firstParent = multiResults[0]->parent;
            for (auto r : multiResults) {
                if (r->kind != firstKind) {
                    multipleDistinctKinds = true;
                }
                if (r->parent != firstParent) {
                    multipleDistinctParents = true;
                }
            }
        }

        auto renderSymbol = [&](const analysis::Symbol* renderSym) {
            std::string dispName = (!dynamicDisplayName.empty() && renderSym->name == sym->name) ? dynamicDisplayName : renderSym->name;
            std::string sig = !renderSym->signature.empty() ? renderSym->signature : (renderSym->typeInfo + (renderSym->typeInfo.empty() ? "" : " ") + dispName);
            std::string contextStr = renderSym->parent ? renderSym->parent->name : "";
            
            std::string md = "```angelscript\n" + sig + "\n```\n"
                           + "**" + dispName + "** — " + getKindName(renderSym->kind)
                           + (!contextStr.empty() ? " " + std::string(s.hoverIn) + " `" + contextStr + "`" : "");
            
            if (!renderSym->docComment.empty()) {
                md += "\n\n" + renderSym->docComment;
            }
            return md;
        };

        if (multipleDistinctKinds || multipleDistinctParents) {
            for (size_t i = 0; i < multiResults.size(); i++) {
                if (i > 0) markdown += "\n\n---\n\n";
                markdown += renderSymbol(multiResults[i]);
            }
        } else {
            markdown = renderSymbol(sym);
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
