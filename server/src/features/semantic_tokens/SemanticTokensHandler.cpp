#include "SemanticTokensHandler.h"
#include <angelscript.h>

namespace angel_lsp {
namespace features {

lsp::requests::TextDocument_SemanticTokens_Full::Result ProcessSemanticTokensFull(
    const lsp::requests::TextDocument_SemanticTokens_Full::Params& req,
    const Document& doc,
    const analysis::SymbolTable& table
) {
    lsp::requests::TextDocument_SemanticTokens_Full::Result res;
    res = lsp::SemanticTokens{};
    auto& tokens = *res;
    
    struct RawToken {
        uint32_t line, col, len, tokenType, tokenModifiers;
    };
    std::vector<RawToken> rawTokens;

    auto getTokenType = [](analysis::SymbolKind kind) -> uint32_t {
        switch (kind) {
            case analysis::SymbolKind::Namespace: return 0;
            case analysis::SymbolKind::Class: return 2;
            case analysis::SymbolKind::Enum: return 3;
            case analysis::SymbolKind::Parameter: return 7;
            case analysis::SymbolKind::Variable: return 8;
            case analysis::SymbolKind::Property: return 9;
            case analysis::SymbolKind::EnumMember: return 10;
            case analysis::SymbolKind::Function: return 12;
            case analysis::SymbolKind::Method: return 13;
            case analysis::SymbolKind::Funcdef: return 1;
            case analysis::SymbolKind::Typedef: return 2; // type
            case analysis::SymbolKind::Mixin: return 2; // type
        }
        return 0; // fallback
    };

    auto processSymbol = [&](auto& self, const analysis::Symbol* sym) -> void {
        if (!sym) return;

        uint32_t line = sym->selectionRange.start.line;
        uint32_t col = sym->selectionRange.start.character;
        uint32_t len = sym->selectionRange.end.character - sym->selectionRange.start.character;
        uint32_t tokenType = getTokenType(sym->kind);

        if (len > 0) {
            rawTokens.push_back({line, col, len, tokenType, 0});
        }

        for (const auto& child : sym->children) {
            self(self, child.get());
        }
    };

    // Iterate over globals
    for (const auto& [name, syms] : table.GetGlobals()) {
        for (const auto& sym : syms) {
            processSymbol(processSymbol, sym.get());
        }
    }

    // Iterate over locals
    for (const auto& sym : table.GetLocals()) {
        processSymbol(processSymbol, sym.get());
    }

    // Sort by line, then col
    std::sort(rawTokens.begin(), rawTokens.end(), [](const RawToken& a, const RawToken& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.col < b.col;
    });

    uint32_t prevLine = 0;
    uint32_t prevCol = 0;

    for (const auto& tok : rawTokens) {
        uint32_t deltaLine = tok.line - prevLine;
        uint32_t deltaCol = (deltaLine == 0) ? tok.col - prevCol : tok.col;

        tokens.data.push_back(deltaLine);
        tokens.data.push_back(deltaCol);
        tokens.data.push_back(tok.len);
        tokens.data.push_back(tok.tokenType);
        tokens.data.push_back(tok.tokenModifiers);

        prevLine = tok.line;
        prevCol = tok.col;
    }

    return res;
}

}
}
