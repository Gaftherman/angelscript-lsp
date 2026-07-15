#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

class asIScriptEngine;

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_Completion::Result ProcessCompletion(
    const lsp::requests::TextDocument_Completion::Params& req,
    const Document& doc,
    const asIScriptEngine* engine
);

}
}
