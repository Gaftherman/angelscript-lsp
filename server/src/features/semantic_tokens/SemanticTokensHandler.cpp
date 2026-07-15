#include "SemanticTokensHandler.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_SemanticTokens_Full::Result ProcessSemanticTokensFull(
    const lsp::requests::TextDocument_SemanticTokens_Full::Params& req,
    const Document& doc,
    const asIScriptEngine* engine
) {
    lsp::requests::TextDocument_SemanticTokens_Full::Result res;
    res = lsp::SemanticTokens{};
    auto& tokens = *res;
    tokens.data = {}; // We will fill this using Tree-Sitter AST
    return res;
}

}
}
