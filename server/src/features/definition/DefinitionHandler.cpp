#include "DefinitionHandler.h"
#include "analysis/SymbolResolver.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_Definition::Result ProcessDefinition(
    const lsp::requests::TextDocument_Definition::Params& req,
    const Document& doc,
    const analysis::SymbolTable& table,
    const asIScriptEngine* engine
) {
    lsp::requests::TextDocument_Definition::Result res;
    
    const analysis::Symbol* sym = analysis::SymbolResolver::ResolveAt(doc, table, req.position.line, req.position.character);
    if (!sym) return {};

    lsp::Location loc;
    loc.uri = lsp::DocumentUri::fromPath(doc.GetUri()); // Currently all symbols are in the same file context for this iteration
    loc.range = sym->selectionRange;

    res = loc;
    return res;
}

}
}
