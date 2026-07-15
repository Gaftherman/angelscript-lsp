#include "HoverHandler.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_Hover::Result ProcessHover(
    const lsp::requests::TextDocument_Hover::Params& req,
    const Document& doc,
    const asIScriptEngine* engine
) {
    lsp::requests::TextDocument_Hover::Result res;
    // Example basic hover
    res = lsp::Hover{};
    auto& hover = *res;
    lsp::MarkupContent markup;
    markup.kind = lsp::MarkupKind::Markdown;
    markup.value = "Hover info for AngelScript";
    hover.contents = markup;
    // We would use our parser/AST to resolve symbol under req.position
    return res;
}

}
}
