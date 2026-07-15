#include "DefinitionHandler.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_Definition::Result ProcessDefinition(
    const lsp::requests::TextDocument_Definition::Params& req,
    const Document& doc,
    const asIScriptEngine* engine
) {
    lsp::requests::TextDocument_Definition::Result res;
    // We would use our parser/AST to resolve symbol under req.position
    // and return Location or LocationLink
    return res;
}

}
}
