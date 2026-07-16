#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticCache.h"
#include "i18n/LspStrings.h"

class asIScriptEngine;

namespace angel_lsp {
namespace features {

void ProcessHover(lsp::requests::TextDocument_Hover::Result& result,
                  const lsp::requests::TextDocument_Hover::Params& req,
                  const Document& doc,
                  const analysis::SymbolTable& table,
                  const analysis::DiagnosticCache* diagCache,
                  i18n::Locale locale,
                  const asIScriptEngine* engine);

}
}
