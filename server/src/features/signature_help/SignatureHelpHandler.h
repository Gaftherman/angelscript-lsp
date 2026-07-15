#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"

class asIScriptEngine;

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_SignatureHelp::Result ProcessSignatureHelp(
    const lsp::requests::TextDocument_SignatureHelp::Params& req,
    const Document& doc,
    const analysis::SymbolTable& table,
    const asIScriptEngine* engine
);

}
}
