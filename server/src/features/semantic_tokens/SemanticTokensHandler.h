#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_SemanticTokens_Full::Result ProcessSemanticTokensFull(
    const lsp::requests::TextDocument_SemanticTokens_Full::Params& req,
    const Document& doc,
    const analysis::SymbolTable& table
);

}
}
