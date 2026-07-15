#include "SignatureHelpHandler.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_SignatureHelp::Result ProcessSignatureHelp(
    const lsp::requests::TextDocument_SignatureHelp::Params& req,
    const Document& doc,
    const asIScriptEngine* engine
) {
    lsp::requests::TextDocument_SignatureHelp::Result res;
    res = lsp::SignatureHelp{};
    auto& help = *res;
    help.signatures = {};
    return res;
}

}
}
