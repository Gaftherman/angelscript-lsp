/**
 * @file SemanticTokensHandler.cpp
 * @brief Implementation of textDocument/semanticTokens/full request processor.
 * @ingroup Features
 */

#include "features/semantic_tokens/SemanticTokensHandler.h"
#include <lsp/messages.h>
#include <algorithm>
#include <string_view>

namespace angel_lsp::features::semantic_tokens
{
    std::vector<std::string> SemanticTokensHandler::GetTokenTypesLegend()
    {
        return {
            "namespace",     // 0
            "type",          // 1
            "class",         // 2
            "enum",          // 3
            "interface",     // 4
            "struct",        // 5
            "typeParameter", // 6
            "parameter",     // 7
            "variable",      // 8
            "property",      // 9
            "enumMember",    // 10
            "event",         // 11
            "function",      // 12
            "method",        // 13
            "macro",         // 14
            "keyword",       // 15
            "modifier",      // 16
            "comment",       // 17
            "string",        // 18
            "number",        // 19
            "regexp",        // 20
            "operator"       // 21
        };
    }

    std::vector<std::string> SemanticTokensHandler::GetTokenModifiersLegend()
    {
        return {
            "declaration",   // 1 << 0
            "readonly",      // 1 << 1
            "static",        // 1 << 2
            "deprecated",    // 1 << 3
            "documentation"  // 1 << 4
        };
    }

    static TokenType SymbolKindToTokenType(analysis::SymbolKind kind)
    {
        switch (kind)
        {
        case analysis::SymbolKind::Namespace:
            return TokenType::Namespace;
        case analysis::SymbolKind::Class:
        case analysis::SymbolKind::Mixin:
        case analysis::SymbolKind::Typedef:
        case analysis::SymbolKind::Funcdef:
            return TokenType::Class;
        case analysis::SymbolKind::Enum:
            return TokenType::Enum;
        case analysis::SymbolKind::Interface:
            return TokenType::Interface;
        case analysis::SymbolKind::Parameter:
            return TokenType::Parameter;
        case analysis::SymbolKind::Variable:
            return TokenType::Variable;
        case analysis::SymbolKind::Property:
            return TokenType::Property;
        case analysis::SymbolKind::EnumMember:
            return TokenType::EnumMember;
        case analysis::SymbolKind::Function:
        case analysis::SymbolKind::Constructor:
        case analysis::SymbolKind::Destructor:
            return TokenType::Function;
        case analysis::SymbolKind::Method:
            return TokenType::Method;
        default:
            return TokenType::Variable;
        }
    }

    static bool IsPrimitiveType(std::string_view type, std::string_view text)
    {
        if (type == "primitive_type" || type == "void" || text == "void" || text == "int" ||
            text == "float" || text == "bool" || text == "double" || text == "uint" ||
            text == "uint8" || text == "uint16" || text == "uint32" || text == "uint64" ||
            text == "int8" || text == "int16" || text == "int32" || text == "int64" || text == "auto")
        {
            return true;
        }
        return false;
    }

    static bool IsKeyword(std::string_view type, std::string_view text)
    {
        if (type == "class" || type == "namespace" || type == "private" || type == "protected" ||
            type == "public" || type == "interface" || type == "mixin" || type == "enum" ||
            type == "typedef" || type == "import" || type == "from" || type == "return" ||
            type == "if" || type == "else" || type == "for" || type == "foreach" ||
            type == "while" || type == "do" || type == "switch" || type == "case" ||
            type == "default" || type == "try" || type == "catch" || type == "break" ||
            type == "continue" || type == "const" || type == "shared" || type == "abstract" ||
            type == "final" || type == "override" || type == "explicit" || type == "cast" ||
            type == "using" || type == "funcdef" || type == "property" || type == "delete" ||
            type == "in" || type == "out" || type == "inout" || type == "get" || type == "set" ||
            type == "is" || type == "not" || type == "and" || type == "or" || type == "xor")
        {
            return true;
        }

        if (text == "class" || text == "namespace" || text == "private" || text == "protected" ||
            text == "public" || text == "interface" || text == "mixin" || text == "enum" ||
            text == "typedef" || text == "import" || text == "from" || text == "return" ||
            text == "if" || text == "else" || text == "for" || text == "foreach" ||
            text == "while" || text == "do" || text == "switch" || text == "case" ||
            text == "default" || text == "try" || text == "catch" || text == "break" ||
            text == "continue" || text == "const" || text == "shared" || text == "abstract" ||
            text == "final" || text == "override" || text == "explicit" || text == "cast" ||
            text == "using" || text == "funcdef" || text == "property" || text == "delete" ||
            text == "in" || text == "out" || text == "inout" || text == "get" || text == "set" ||
            text == "is" || text == "not" || text == "and" || text == "or" || text == "xor" ||
            text == "true" || text == "false" || text == "null")
        {
            return true;
        }

        return false;
    }

    static TokenType InferTokenTypeFromAST(TSNode node, const Document &doc)
    {
        TSNode parent = ts_node_parent(node);
        if (ts_node_is_null(parent))
        {
            return TokenType::Variable;
        }

        std::string_view parentType = ts_node_type(parent);

        // Check if parent is a function or method call
        if (parentType == "call_expression")
        {
            TSNode funcNode = ts_node_child_by_field_name(parent, "function", 8);
            if (!ts_node_is_null(funcNode))
            {
                if (ts_node_type(funcNode) == std::string_view("member_expression"))
                {
                    return TokenType::Method;
                }
                return TokenType::Function;
            }
        }

        // Check if node is member field access: e.g. obj.member
        if (parentType == "member_expression")
        {
            TSNode memberNode = ts_node_child_by_field_name(parent, "member", 6);
            if (ts_node_eq(memberNode, node))
            {
                TSNode gParent = ts_node_parent(parent);
                if (!ts_node_is_null(gParent) && ts_node_type(gParent) == std::string_view("call_expression"))
                {
                    return TokenType::Method;
                }
                return TokenType::Property;
            }
        }

        // Check if node is parameter
        if (parentType == "parameter")
        {
            return TokenType::Parameter;
        }

        // Walk up AST scope
        TSNode curr = parent;
        while (!ts_node_is_null(curr))
        {
            std::string_view cType = ts_node_type(curr);

            if (cType == "class_declaration" || cType == "class_body" || cType == "mixin_declaration" || cType == "interface_declaration")
            {
                if (parentType == "variable_declarator" || parentType == "variable_declaration")
                {
                    return TokenType::Property;
                }
                if (parentType == "func_declaration")
                {
                    return TokenType::Method;
                }
            }
            else if (cType == "namespace_declaration")
            {
                TSNode nameNode = ts_node_child_by_field_name(curr, "name", 4);
                if (ts_node_eq(nameNode, node))
                {
                    return TokenType::Namespace;
                }
            }
            else if (cType == "scoped_type" || cType == "scoped_identifier")
            {
                uint32_t count = ts_node_child_count(curr);
                if (count > 0)
                {
                    TSNode lastChild = ts_node_child(curr, count - 1);
                    if (ts_node_eq(lastChild, node))
                    {
                        return TokenType::Class;
                    }
                }
                return TokenType::Namespace;
            }

            curr = ts_node_parent(curr);
        }

        return TokenType::Variable;
    }

    static void TraverseForSemanticTokens(
        TSNode node,
        const Document &doc,
        const analysis::SymbolTable &table,
        std::vector<SemanticToken> &tokens)
    {
        if (ts_node_is_null(node))
        {
            return;
        }

        std::string_view type = ts_node_type(node);
        std::string_view text = doc.SourceAt(node);
        TSPoint startPoint = ts_node_start_point(node);
        TSPoint endPoint = ts_node_end_point(node);

        if (type == "identifier")
        {
            if (startPoint.row == endPoint.row && endPoint.column > startPoint.column)
            {
                uint32_t line = startPoint.row;
                uint32_t startChar = startPoint.column;
                uint32_t length = endPoint.column - startPoint.column;

                if (const analysis::Symbol *sym = analysis::SymbolResolver::ResolveAt(doc, table, line, startChar))
                {
                    TokenType tType = SymbolKindToTokenType(sym->kind);
                    uint32_t modifiers = static_cast<uint32_t>(TokenModifier::None);

                    if (sym->selectionRange.start.line == line && sym->selectionRange.start.character == startChar)
                    {
                        modifiers |= static_cast<uint32_t>(TokenModifier::Declaration);
                    }

                    if (!sym->docComment.empty())
                    {
                        modifiers |= static_cast<uint32_t>(TokenModifier::Documentation);
                    }

                    tokens.push_back({line, startChar, length, static_cast<uint32_t>(tType), modifiers});
                }
                else if (!IsPrimitiveType(type, text) && !IsKeyword(type, text))
                {
                    TokenType inferredType = InferTokenTypeFromAST(node, doc);
                    tokens.push_back({line, startChar, length, static_cast<uint32_t>(inferredType), 0});
                }
            }
        }
        else if (type == "preproc_directive")
        {
            std::string_view dirText = text;
            if (startPoint.row == endPoint.row && endPoint.column > startPoint.column)
            {
                size_t spacePos = dirText.find_first_of(" \t");
                if (spacePos != std::string_view::npos)
                {
                    size_t restStart = dirText.find_first_not_of(" \t", spacePos);
                    if (restStart != std::string_view::npos)
                    {
                        std::string_view rest = dirText.substr(restStart);
                        if (!rest.starts_with('"') && !rest.starts_with('<'))
                        {
                            uint32_t restStartCol = startPoint.column + static_cast<uint32_t>(restStart);
                            tokens.push_back({startPoint.row, restStartCol, static_cast<uint32_t>(rest.size()), static_cast<uint32_t>(TokenType::Macro), 0});
                        }
                    }
                }
            }
        }

        uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            TraverseForSemanticTokens(ts_node_child(node, i), doc, table, tokens);
        }
    }

    std::vector<uint32_t> SemanticTokensHandler::ProvideSemanticTokens(
        const Document &doc,
        const analysis::SymbolTable &table)
    {
        std::vector<SemanticToken> rawTokens;
        TraverseForSemanticTokens(doc.RootNode(), doc, table, rawTokens);

        // Sort tokens by line, then by start character
        std::sort(rawTokens.begin(), rawTokens.end(), [](const SemanticToken &a, const SemanticToken &b)
        {
            if (a.line != b.line)
            {
                return a.line < b.line;
            }
            return a.startChar < b.startChar;
        });

        // Deduplicate tokens at exact same position
        std::vector<SemanticToken> uniqueTokens;
        uniqueTokens.reserve(rawTokens.size());
        for (const auto &tok : rawTokens)
        {
            if (!uniqueTokens.empty() && uniqueTokens.back().line == tok.line && uniqueTokens.back().startChar == tok.startChar)
            {
                continue;
            }
            uniqueTokens.push_back(tok);
        }

        // Encode deltas into LSP 5-integer tuple output format
        std::vector<uint32_t> encodedData;
        encodedData.reserve(uniqueTokens.size() * 5);

        uint32_t prevLine = 0;
        uint32_t prevChar = 0;

        for (const auto &tok : uniqueTokens)
        {
            uint32_t deltaLine = tok.line - prevLine;
            uint32_t deltaStartChar = (deltaLine == 0) ? (tok.startChar - prevChar) : tok.startChar;

            encodedData.push_back(deltaLine);
            encodedData.push_back(deltaStartChar);
            encodedData.push_back(tok.length);
            encodedData.push_back(tok.tokenType);
            encodedData.push_back(tok.tokenModifiers);

            prevLine = tok.line;
            prevChar = tok.startChar;
        }

        return encodedData;
    }

    lsp::requests::TextDocument_SemanticTokens_Full::Result ProcessSemanticTokensFull(
        const lsp::requests::TextDocument_SemanticTokens_Full::Params &params,
        const Document &doc,
        const analysis::SymbolTable &table)
    {
        lsp::requests::TextDocument_SemanticTokens_Full::Result result;
        lsp::SemanticTokens st;
        st.data = SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        result = st;
        return result;
    }
} // namespace angel_lsp::features::semantic_tokens
