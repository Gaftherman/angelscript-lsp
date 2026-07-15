#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

class asIScriptEngine;

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_Hover::Result ProcessHover(
    const lsp::requests::TextDocument_Hover::Params& req,
    const Document& doc,
    const asIScriptEngine* engine
);

}
}
