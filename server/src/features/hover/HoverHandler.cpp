#include "HoverHandler.h"
#include "analysis/SymbolResolver.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

static std::string KindName(analysis::SymbolKind kind) {
    switch (kind) {
        case analysis::SymbolKind::Variable: return "Variable";
        case analysis::SymbolKind::Function: return "Function";
        case analysis::SymbolKind::Class: return "Class";
        case analysis::SymbolKind::Namespace: return "Namespace";
        case analysis::SymbolKind::Parameter: return "Parameter";
        case analysis::SymbolKind::Property: return "Property";
        case analysis::SymbolKind::Method: return "Method";
        case analysis::SymbolKind::Enum: return "Enum";
        case analysis::SymbolKind::EnumMember: return "Enum Member";
        case analysis::SymbolKind::Interface: return "Interface";
        case analysis::SymbolKind::Funcdef: return "Funcdef";
        default: return "Symbol";
    }
}

static std::string QueryEngineFunction(const asIScriptEngine* engine, const std::string& name) {
    if (!engine) return "";
    for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++) {
        asIScriptFunction* func = engine->GetGlobalFunctionByIndex(i);
        if (func && std::string(func->GetName()) == name) {
            std::string decl = func->GetDeclaration(true, true, true);
            return "```angelscript\n" + decl + "\n```\n**" + name + "** — Built-in Function";
        }
    }
    return "";
}

static std::string QueryEngineType(const asIScriptEngine* engine, const std::string& name) {
    if (!engine) return "";
    int typeId = engine->GetTypeIdByDecl(name.c_str());
    if (typeId >= 0) {
        asITypeInfo* type = engine->GetTypeInfoById(typeId);
        if (type) {
            return "```angelscript\nclass " + std::string(type->GetName()) + "\n```\n**" + name + "** — Built-in Type";
        }
    }
    return "";
}

lsp::requests::TextDocument_Hover::Result ProcessHover(
    const lsp::requests::TextDocument_Hover::Params& req,
    const Document& doc,
    const analysis::SymbolTable& table,
    const asIScriptEngine* engine
) {
    lsp::requests::TextDocument_Hover::Result res;
    
    uint32_t line = req.position.line;
    uint32_t col = req.position.character;
    
    std::string markdown = "";
    
    const analysis::Symbol* sym = analysis::SymbolResolver::ResolveAt(doc, table, line, col);
    if (sym != nullptr)
    {
        auto getFullName = [](const analysis::Symbol* s) -> std::string {
            if (!s || !s->parent) return "";
            std::string name = s->parent->name;
            const analysis::Symbol* p = s->parent->parent;
            while (p && !p->name.empty()) {
                name = p->name + "::" + name;
                p = p->parent;
            }
            return name;
        };

        std::string sig = !sym->signature.empty() ? sym->signature : (sym->typeInfo + (sym->typeInfo.empty() ? "" : " ") + sym->name);
        std::string containerName = getFullName(sym);
        
        markdown = "```angelscript\n" + sig + "\n```\n"
                 + "**" + sym->name + "** — " + KindName(sym->kind)
                 + (!containerName.empty() ? " in `" + containerName + "`" : "");
                 
        if (!sym->docComment.empty()) {
            markdown += "\n\n" + sym->docComment;
        }
    }
    else
    {
        TSNode nodeUnder = doc.NodeAt(line, col);
        if (!ts_node_is_null(nodeUnder))
        {
            std::string_view sv = doc.SourceAt(nodeUnder);
            std::string name(sv.begin(), sv.end());
            
            markdown = QueryEngineFunction(engine, name);
            if (markdown.empty()) {
                markdown = QueryEngineType(engine, name);
            }
        }
    }

    if (markdown.empty()) return {};

    res = lsp::Hover{};
    auto& hover = *res;
    lsp::MarkupContent markup;
    markup.kind = lsp::MarkupKind::Markdown;
    markup.value = markdown;
    hover.contents = markup;
    
    return res;
}

}
}
