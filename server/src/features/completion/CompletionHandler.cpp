#include "CompletionHandler.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_Completion::Result ProcessCompletion(
    const lsp::requests::TextDocument_Completion::Params& req,
    const Document& doc,
    const asIScriptEngine* engine
) {
    lsp::requests::TextDocument_Completion::Result res;
    // Basic completion example
    std::vector<lsp::CompletionItem> items;
    
    lsp::CompletionItem item;
    item.label = "print";
    item.kind = lsp::CompletionItemKind::Function;
    item.detail = "void print(const string&in)";
    
    items.push_back(item);
    res = items;
    
    return res;
}

}
}
