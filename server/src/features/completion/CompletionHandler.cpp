#include "CompletionHandler.h"
#include "analysis/SymbolTable.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

enum class CompletionContextType {
    Global,
    Member,
    Namespace
};

struct CompletionContext {
    CompletionContextType type;
    std::string scopeName;
};

static size_t GetByteOffset(const std::string& text, uint32_t line, uint32_t col) {
    size_t offset = 0;
    uint32_t currentLine = 0;
    while (currentLine < line && offset < text.length()) {
        if (text[offset] == '\n') {
            currentLine++;
        }
        offset++;
    }
    return std::min(offset + col, text.length());
}

static std::string ReadIdentifierBefore(const std::string& text, size_t pos) {
    while (pos > 0 && std::isspace(text[pos - 1])) pos--;
    size_t end = pos;
    while (pos > 0 && (std::isalnum(text[pos - 1]) || text[pos - 1] == '_')) {
        pos--;
    }
    return text.substr(pos, end - pos);
}

static CompletionContext AnalyzeContext(const std::string& text, uint32_t line, uint32_t col) {
    size_t pos = GetByteOffset(text, line, col);
    
    // Skip whitespace backward from cursor to find trigger char
    while (pos > 0 && std::isspace(text[pos - 1])) {
        pos--;
    }
    
    if (pos > 0 && text[pos - 1] == '.') {
        std::string objectName = ReadIdentifierBefore(text, pos - 1);
        return {CompletionContextType::Member, objectName};
    }
    
    if (pos > 1 && text[pos - 1] == ':' && text[pos - 2] == ':') {
        std::string nsName = ReadIdentifierBefore(text, pos - 2);
        return {CompletionContextType::Namespace, nsName};
    }
    
    return {CompletionContextType::Global, ""};
}

static lsp::CompletionItemKind SymbolKindToCompletionKind(analysis::SymbolKind kind) {
    switch (kind) {
        case analysis::SymbolKind::Variable:   return lsp::CompletionItemKind::Variable;
        case analysis::SymbolKind::Function:   return lsp::CompletionItemKind::Function;
        case analysis::SymbolKind::Class:      return lsp::CompletionItemKind::Class;
        case analysis::SymbolKind::Namespace:  return lsp::CompletionItemKind::Module;
        case analysis::SymbolKind::Parameter:  return lsp::CompletionItemKind::Variable;
        case analysis::SymbolKind::Property:   return lsp::CompletionItemKind::Property;
        case analysis::SymbolKind::Method:     return lsp::CompletionItemKind::Method;
        case analysis::SymbolKind::Enum:       return lsp::CompletionItemKind::Enum;
        case analysis::SymbolKind::EnumMember: return lsp::CompletionItemKind::EnumMember;
        case analysis::SymbolKind::Interface:  return lsp::CompletionItemKind::Interface;
        case analysis::SymbolKind::Funcdef:    return lsp::CompletionItemKind::Function;
        case analysis::SymbolKind::Mixin:      return lsp::CompletionItemKind::Class;
        case analysis::SymbolKind::Typedef:    return lsp::CompletionItemKind::Struct;
        default: return lsp::CompletionItemKind::Text;
    }
}

static void AddEngineCompletionItems(const asIScriptEngine* engine, std::vector<lsp::CompletionItem>& items) {
    if (!engine) return;
    
    for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++) {
        asIScriptFunction* func = engine->GetGlobalFunctionByIndex(i);
        if (func) {
            lsp::CompletionItem item;
            item.label = func->GetName();
            item.kind = lsp::CompletionItemKind::Function;
            item.detail = func->GetDeclaration(false, true, true);
            items.push_back(item);
        }
    }
    
    for (asUINT i = 0; i < engine->GetObjectTypeCount(); i++) {
        asITypeInfo* type = engine->GetObjectTypeByIndex(i);
        if (type) {
            lsp::CompletionItem item;
            item.label = type->GetName();
            item.kind = lsp::CompletionItemKind::Class;
            item.detail = "Built-in Type";
            items.push_back(item);
        }
    }
}

lsp::requests::TextDocument_Completion::Result ProcessCompletion(
    const lsp::requests::TextDocument_Completion::Params& req,
    const Document& doc,
    const analysis::SymbolTable& table,
    const asIScriptEngine* engine
) {
    lsp::requests::TextDocument_Completion::Result res;
    std::vector<lsp::CompletionItem> items;
    
    CompletionContext ctx = AnalyzeContext(doc.GetText(), req.position.line, req.position.character);
    
    if (ctx.type == CompletionContextType::Global) {
        for (const auto& [name, sym] : table.GetGlobals()) {
            lsp::CompletionItem item;
            item.label = sym->name;
            item.kind = SymbolKindToCompletionKind(sym->kind);
            item.detail = !sym->signature.empty() ? sym->signature : (sym->typeInfo + (sym->typeInfo.empty() ? "" : " ") + sym->name);
            items.push_back(item);
        }
        AddEngineCompletionItems(engine, items);
        
        const char* keywords[] = {
            "and", "abstract", "auto", "bool", "break", "case", "cast", "class", "const", "continue", "default", "do", "double", "else", "enum", "explicit", "external", "false", "final", "float", "for", "from", "funcdef", "get", "if", "import", "in", "inout", "int", "int8", "int16", "int32", "int64", "interface", "is", "mixin", "namespace", "not", "null", "or", "out", "override", "private", "property", "protected", "return", "set", "shared", "stat", "switch", "this", "true", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "void", "while", "xor"
        };
        for (const char* kw : keywords) {
            lsp::CompletionItem item;
            item.label = kw;
            item.kind = lsp::CompletionItemKind::Keyword;
            items.push_back(item);
        }
    } 
    else if (ctx.type == CompletionContextType::Member) {
        const analysis::Symbol* objectSym = table.FindFirst(ctx.scopeName);
        if (objectSym) {
            // Clean type name (e.g. "Enemy@" -> "Enemy")
            std::string typeName = objectSym->typeInfo;
            if (!typeName.empty() && typeName.back() == '@') {
                typeName.pop_back();
            }
            
            auto members = table.FindInContainer(typeName);
            for (const auto& sym : members) {
                lsp::CompletionItem item;
                item.label = sym->name;
                item.kind = SymbolKindToCompletionKind(sym->kind);
                item.detail = !sym->signature.empty() ? sym->signature : (sym->typeInfo + (sym->typeInfo.empty() ? "" : " ") + sym->name);
                items.push_back(item);
            }
            
            // Engine fallback for members
            if (engine) {
                int typeId = engine->GetTypeIdByDecl(typeName.c_str());
                if (typeId >= 0) {
                    asITypeInfo* type = engine->GetTypeInfoById(typeId);
                    if (type) {
                        for (asUINT i = 0; i < type->GetMethodCount(); i++) {
                            asIScriptFunction* func = type->GetMethodByIndex(i);
                            if (func) {
                                lsp::CompletionItem item;
                                item.label = func->GetName();
                                item.kind = lsp::CompletionItemKind::Method;
                                item.detail = func->GetDeclaration(false, true, true);
                                items.push_back(item);
                            }
                        }
                        for (asUINT i = 0; i < type->GetPropertyCount(); i++) {
                            const char* name = nullptr;
                            type->GetProperty(i, &name);
                            if (name) {
                                lsp::CompletionItem item;
                                item.label = name;
                                item.kind = lsp::CompletionItemKind::Property;
                                item.detail = type->GetPropertyDeclaration(i, false);
                                items.push_back(item);
                            }
                        }
                    }
                }
            }
        }
    }
    else if (ctx.type == CompletionContextType::Namespace) {
        auto members = table.FindInContainer(ctx.scopeName);
        for (const auto& sym : members) {
            lsp::CompletionItem item;
            item.label = sym->name;
            item.kind = SymbolKindToCompletionKind(sym->kind);
            item.detail = !sym->signature.empty() ? sym->signature : (sym->typeInfo + (sym->typeInfo.empty() ? "" : " ") + sym->name);
            items.push_back(item);
        }
    }
    
    res = items;
    return res;
}

}
}
