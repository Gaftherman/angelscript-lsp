#include "features/hover/HoverHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SignatureFormatter.h"
#include "analysis/DocComment.h"
#include "utils/Utils.h"
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_set>

namespace angel_lsp::features
{
    namespace
    {

        const analysis::Scope *FindScopeDeclaringDefinition(const analysis::Scope *current, const analysis::LocalDefinition &def)
        {
            if (!current)
            {
                return nullptr;
            }

            for (const auto &d : current->definitions)
            {
                if (d.name == def.name &&
                    d.startLine == def.startLine &&
                    d.startCharacter == def.startCharacter)
                {
                    return current;
                }
            }

            for (const auto &child : current->children)
            {
                if (const auto *found = FindScopeDeclaringDefinition(child.get(), def))
                {
                    return found;
                }
            }

            return nullptr;
        }

        std::vector<std::string> SplitLines(const std::string &str)
        {
            std::vector<std::string> lines;
            std::string line;
            std::istringstream stream(str);
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.push_back(line);
            }
            return lines;
        }

        std::string Trim(const std::string &str)
        {
            size_t first = str.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return "";
            }
            size_t last = str.find_last_not_of(" \t\r\n");
            return str.substr(first, (last - first + 1));
        }

        std::string FormatFunctionSignature(const analysis::Symbol &sym)
        {
            return analysis::FormatFunctionDeclaration(sym);
        }

        std::string FormatClassSignature(const analysis::Symbol &sym)
        {
            return analysis::FormatTypeDeclaration(sym);
        }

        /** @brief Squeezes runs of whitespace into single spaces so a declaration that was wrapped
         *         across several source lines still renders as one hover line. */
        std::string CollapseWhitespace(const std::string &text)
        {
            std::string result;
            result.reserve(text.size());
            bool pendingSpace = false;

            for (const char c : text)
            {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    pendingSpace = !result.empty();
                    continue;
                }
                if (pendingSpace)
                {
                    result += ' ';
                    pendingSpace = false;
                }
                result += c;
            }
            return result;
        }

        /** @brief Reads a declaration back verbatim from the source at the position it was declared.
         *  @param root Root node of the document tree.
         *  @param sourceCode Document source text.
         *  @param line Zero-based line of the declared identifier.
         *  @param character Zero-based column of the declared identifier.
         *  @param wantedNodeType AST node type to climb to, e.g. "parameter".
         *  @return Whitespace-collapsed declaration text, or an empty string if not found.
         *  @note LocalDefinition only records a type for variables, so a parameter hovered at a use
         *        site inside the body has nothing to show. The declaration position it does record
         *        is enough to find the declaring node and read it back with every modifier the user
         *        wrote ('const', '@', '&in'/'&out'/'&inout') still attached. */
        std::string ExtractDeclarationTextAt(TSNode root,
                                             const std::string &sourceCode,
                                             uint32_t line,
                                             uint32_t character,
                                             std::string_view wantedNodeType)
        {
            if (ts_node_is_null(root))
            {
                return "";
            }

            const TSPoint point{ line, character };
            TSNode current = ts_node_descendant_for_point_range(root, point, point);

            for (int depth = 0; depth < 8 && !ts_node_is_null(current); ++depth, current = ts_node_parent(current))
            {
                if (std::string_view(ts_node_type(current)) != wantedNodeType)
                {
                    continue;
                }

                const uint32_t start = ts_node_start_byte(current);
                const uint32_t end = ts_node_end_byte(current);
                if (start < end && end <= sourceCode.size())
                {
                    return CollapseWhitespace(sourceCode.substr(start, end - start));
                }
                break;
            }
            return "";
        }

        /** @brief Renders the hover line for a variable-like symbol, tagged with its role.
         *  @param sym Symbol of type Variable or Property.
         *  @param role Prefix shown to the user, e.g. "(property) " or "(global variable) ". */
        std::string FormatVariableSignature(const analysis::Symbol &sym, const char *role)
        {
            if (sym.type != analysis::SymbolType::Variable)
            {
                return std::string(role) + sym.name;
            }
            return std::string(role) + analysis::FormatVariableDeclaration(sym.GetVariable(), sym.name);
        }

        /** @brief Renders the single hover line that describes a symbol of any kind. */
        std::string FormatDeclarationText(const analysis::Symbol &sym)
        {
            switch (sym.type)
            {
            case analysis::SymbolType::Function:
            case analysis::SymbolType::Funcdef:
                return FormatFunctionSignature(sym);
            case analysis::SymbolType::Class:
            case analysis::SymbolType::Interface:
                return FormatClassSignature(sym);
            case analysis::SymbolType::Enum:
                return "enum " + sym.name;
            case analysis::SymbolType::Variable:
                return FormatVariableSignature(sym, sym.containerName.empty() ? "(global variable) " : "(property) ");
            case analysis::SymbolType::Typedef:
                return "typedef " + sym.GetTypedef().baseType + " " + sym.name;
            case analysis::SymbolType::Namespace:
                return "namespace " + sym.name;
            case analysis::SymbolType::Property:
                return "(property) " + sym.name;
            default:
                return sym.name;
            }
        }

        /** @brief Drops symbols that are the same declaration seen twice.
         *  @note A file reachable under two URI spellings (workspace scan vs. client didOpen) used
         *        to be collected once per spelling, which showed every overload twice in the hover.
         *        The server de-duplicates at index time; this is the last line of defence so a
         *        stale duplicate can never reach the user. */
        void RemoveDuplicateSymbols(std::vector<analysis::Symbol> &symbols)
        {
            std::vector<analysis::Symbol> unique;
            unique.reserve(symbols.size());

            for (auto &sym : symbols)
            {
                const bool alreadyPresent = std::any_of(unique.begin(), unique.end(),
                    [&sym](const analysis::Symbol &kept)
                    {
                        return kept.type == sym.type &&
                               kept.name == sym.name &&
                               kept.qualifiedName == sym.qualifiedName &&
                               kept.startLine == sym.startLine &&
                               kept.startCharacter == sym.startCharacter &&
                               FormatDeclarationText(kept) == FormatDeclarationText(sym);
                    });

                if (!alreadyPresent)
                {
                    unique.push_back(std::move(sym));
                }
            }

            symbols = std::move(unique);
        }
    }

    namespace
    {
        bool ExtractHoverNode(TSNode rootNode, const std::string &sourceCode, uint32_t line, uint32_t character,
                              TSNode &outNode, std::string &outText, lsp::Range &outRange)
        {
            TSPoint point = { line, character };
            TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);
            if (ts_node_is_null(node))
            {
                return false;
            }

            auto tryExtract = [&](TSNode n) -> bool
            {
                if (ts_node_is_null(n)) return false;
                uint32_t sb = ts_node_start_byte(n);
                uint32_t eb = ts_node_end_byte(n);
                if (sb >= sourceCode.size() || eb > sourceCode.size() || sb >= eb) return false;

                std::string txt = sourceCode.substr(sb, eb - sb);
                std::string_view type = ts_node_type(n);

                if (type == "identifier" || type == "scoped_identifier" || type == "primitive_type" ||
                    analysis::IsPrimitiveTypeName(txt) || analysis::IsReservedKeyword(txt))
                {
                    outNode = n;
                    outText = txt;
                    TSPoint sp = ts_node_start_point(n);
                    TSPoint ep = ts_node_end_point(n);
                    outRange = lsp::Range{ { sp.row, sp.column }, { ep.row, ep.column } };
                    return true;
                }

                // Check if text is a word/identifier
                if (!txt.empty() && (isalpha(static_cast<unsigned char>(txt[0])) || txt[0] == '_'))
                {
                    bool allWord = true;
                    for (char c : txt)
                    {
                        if (!isalnum(static_cast<unsigned char>(c)) && c != '_')
                        {
                            allWord = false;
                            break;
                        }
                    }
                    if (allWord)
                    {
                        outNode = n;
                        outText = txt;
                        TSPoint sp = ts_node_start_point(n);
                        TSPoint ep = ts_node_end_point(n);
                        outRange = lsp::Range{ { sp.row, sp.column }, { ep.row, ep.column } };
                        return true;
                    }
                }
                return false;
            };

            if (tryExtract(node)) return true;

            // Try parent
            TSNode parent = ts_node_parent(node);
            if (tryExtract(parent)) return true;

            // Try prev character if at end of word
            if (character > 0)
            {
                TSPoint prevPt = { line, character - 1 };
                TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPt, prevPt);
                if (tryExtract(prevNode)) return true;
                if (!ts_node_is_null(prevNode) && tryExtract(ts_node_parent(prevNode))) return true;
            }

            return false;
        }
    }

    std::optional<lsp::Hover> GetHover(const HoverRequest &request)
    {
        if (!request.tree || request.sourceCode.empty())
        {
            return std::nullopt;
        }

        TSNode rootNode = ts_tree_root_node(request.tree);
        TSNode node{};
        std::string nodeText;
        lsp::Range range{};

        if (!ExtractHoverNode(rootNode, request.sourceCode, request.position.line, request.position.character, node, nodeText, range))
        {
            return std::nullopt;
        }

        // 1. Primitive type check
        if (analysis::IsPrimitiveTypeName(nodeText))
        {
            std::string md = "```angelscript\n(primitive type) " + nodeText + "\n```";
            return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), md }, range };
        }

        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        TSNode parent = ts_node_parent(node);

        // Check if cursor node is the member child of a member_expression (e.g. "prop" in "obj.prop")
        bool isMemberChildOfExpression = false;
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
        {
            TSNode memNode = ts_node_child_by_field_name(parent, "member", 6);
            if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node)))
            {
                isMemberChildOfExpression = true;
            }
        }

        auto resolveMemberAccess = [&]() -> std::optional<lsp::Hover>
        {
            if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
            {
                return std::nullopt;
            }

            TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
            if (ts_node_is_null(objectNode))
            {
                return std::nullopt;
            }

            uint32_t objStart = ts_node_start_byte(objectNode);
            uint32_t objEnd = ts_node_end_byte(objectNode);
            if (objStart >= request.sourceCode.size() || objEnd > request.sourceCode.size() || objStart >= objEnd)
            {
                return std::nullopt;
            }

            std::string objText = request.sourceCode.substr(objStart, objEnd - objStart);
            std::string receiverTypeName;

            if (objText == "this")
            {
                // Look for enclosing class from symbols
                request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.type == analysis::SymbolType::Class && sym.fileUri == request.uri)
                        {
                            if (request.position.line >= sym.startLine && request.position.line <= sym.endLine)
                            {
                                receiverTypeName = sym.name;
                            }
                        }
                    }
                });
            }
            else if (rootScope)
            {
                const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
                if (scope)
                {
                    const analysis::LocalDefinition *objDef = analysis::ResolveInScope(scope, objText);
                    if (objDef && !objDef->typeName.empty())
                    {
                        receiverTypeName = analysis::CleanBaseType(objDef->typeName);
                    }
                }
            }

            if (receiverTypeName.empty())
            {
                auto globSyms = request.symbolTable.FindSymbols(objText);
                for (const auto &sym : globSyms)
                {
                    if (sym.type == analysis::SymbolType::Variable)
                    {
                        const auto &var = sym.GetVariable();
                        if (!var.typeName.empty())
                        {
                            receiverTypeName = analysis::CleanBaseType(var.typeName);
                            break;
                        }
                    }
                }
            }

            if (!receiverTypeName.empty())
            {
                std::vector<analysis::Symbol> memberSymbols;
                auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, request.symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    std::string qualifiedMember = typeName + "::" + nodeText;
                    auto found = request.symbolTable.FindSymbols(qualifiedMember);
                    if (!found.empty())
                    {
                        memberSymbols = std::move(found);
                        break;
                    }
                }

                if (!memberSymbols.empty())
                {
                    RemoveDuplicateSymbols(memberSymbols);

                    std::ostringstream oss;
                    oss << "```angelscript\n";
                    for (size_t i = 0; i < memberSymbols.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << "\n";
                        }
                        oss << FormatDeclarationText(memberSymbols[i]);
                    }
                    oss << "\n```";

                    std::string doc = analysis::ExtractDocComment(request.sourceCode, memberSymbols[0].startLine);
                    if (!doc.empty())
                    {
                        oss << "\n\n" << doc;
                    }

                    return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
                }
            }

            return std::nullopt;
        };

        // If cursor is on the member child of obj.prop, member access resolution takes precedence over local scope!
        if (isMemberChildOfExpression)
        {
            auto memberHover = resolveMemberAccess();
            if (memberHover.has_value())
            {
                return memberHover;
            }
        }

        // 2. Local Scope Resolution (Variables and Parameters in Function Body)
        if (rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def && (def->kind == analysis::LocalDefinitionKind::Parameter ||
                            def->kind == analysis::LocalDefinitionKind::Variable))
                {
                    std::string typeName = def->typeName;
                    if (typeName.empty())
                    {
                        TSNode cur = node;
                        for (int i = 0; i < 4 && !ts_node_is_null(cur); ++i, cur = ts_node_parent(cur))
                        {
                            std::string_view cType = ts_node_type(cur);
                            if (cType == "parameter" || cType == "variable_declaration")
                            {
                                TSNode typeNode = ts_node_child_by_field_name(cur, "param_type", 10);
                                if (ts_node_is_null(typeNode))
                                {
                                    typeNode = ts_node_child_by_field_name(cur, "var_type", 8);
                                }
                                if (ts_node_is_null(typeNode))
                                {
                                    typeNode = ts_node_child_by_field_name(cur, "type", 4);
                                }
                                if (!ts_node_is_null(typeNode))
                                {
                                    uint32_t tStart = ts_node_start_byte(typeNode);
                                    uint32_t tEnd = ts_node_end_byte(typeNode);
                                    if (tStart < request.sourceCode.size() && tEnd <= request.sourceCode.size() && tStart < tEnd)
                                    {
                                        typeName = request.sourceCode.substr(tStart, tEnd - tStart);
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    std::ostringstream oss;
                    oss << "```angelscript\n";
                    switch (def->kind)
                    {
                    case analysis::LocalDefinitionKind::Parameter:
                    {
                        oss << "(parameter) ";
                        // Preferred over the recorded type: only the declaration itself carries the
                        // reference direction ('&in'/'&out'/'&inout') the user wrote.
                        const std::string declText = ExtractDeclarationTextAt(
                            rootNode, request.sourceCode, def->startLine, def->startCharacter, "parameter");
                        if (!declText.empty())
                        {
                            oss << declText;
                        }
                        else
                        {
                            if (!typeName.empty())
                            {
                                oss << typeName << " ";
                            }
                            oss << def->name;
                        }
                        break;
                    }
                    case analysis::LocalDefinitionKind::Variable:
                    {
                        const analysis::Scope *declaringScope = FindScopeDeclaringDefinition(rootScope.get(), *def);
                        bool isFileScope = (declaringScope == rootScope.get() || (declaringScope && declaringScope->parent == nullptr));

                        if (isFileScope)
                        {
                            const analysis::Symbol *globalSym = nullptr;
                            auto candidates = request.symbolTable.FindSymbols(def->name);
                            for (const auto &cand : candidates)
                            {
                                if (cand.type == analysis::SymbolType::Variable && cand.containerName.empty() && cand.fileUri == request.uri)
                                {
                                    globalSym = &cand;
                                    break;
                                }
                            }
                            if (!globalSym)
                            {
                                for (const auto &cand : candidates)
                                {
                                    if (cand.type == analysis::SymbolType::Variable && cand.containerName.empty())
                                    {
                                        globalSym = &cand;
                                        break;
                                    }
                                }
                            }

                            if (globalSym)
                            {
                                std::string sig = FormatVariableSignature(*globalSym, "(global variable) ");
                                oss << sig;
                            }
                            else
                            {
                                oss << "(global variable) ";
                                if (!typeName.empty())
                                {
                                    oss << typeName << " ";
                                }
                                oss << def->name;
                            }
                        }
                        else
                        {
                            oss << "(local variable) ";
                            if (!typeName.empty())
                            {
                                oss << typeName << " ";
                            }
                            oss << def->name;
                        }
                        break;
                    }
                    default:
                        oss << def->name;
                        break;
                    }
                    oss << "\n```";

                    std::string doc = analysis::ExtractDocComment(request.sourceCode, def->startLine);
                    if (!doc.empty())
                    {
                        oss << "\n\n" << doc;
                    }

                    return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
                }
            }
        }

        // 3. Fallback Member Access Resolution if not already resolved
        if (!isMemberChildOfExpression)
        {
            auto memberHover = resolveMemberAccess();
            if (memberHover.has_value())
            {
                return memberHover;
            }
        }

        // 4. Container / Scoped / Global Symbol Lookup
        auto symbols = analysis::FindSymbolsInScope(nodeText, node, request.sourceCode, request.symbolTable);
        if (symbols.empty())
        {
            // Try resolving if inside a scoped identifier
            if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "scoped_identifier")
            {
                uint32_t pStart = ts_node_start_byte(parent);
                uint32_t pEnd = ts_node_end_byte(parent);
                if (pStart < request.sourceCode.size() && pEnd <= request.sourceCode.size())
                {
                    std::string scopedText = request.sourceCode.substr(pStart, pEnd - pStart);
                    symbols = analysis::FindSymbolsInScope(scopedText, node, request.sourceCode, request.symbolTable);
                    if (!symbols.empty())
                    {
                        TSPoint pStartPt = ts_node_start_point(parent);
                        TSPoint pEndPt = ts_node_end_point(parent);
                        range = lsp::Range{ { pStartPt.row, pStartPt.column }, { pEndPt.row, pEndPt.column } };
                    }
                }
            }
        }

        // Fallback to local scope definition (e.g. Field or non-function variable) if not found in SymbolTable
        if (symbols.empty() && rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def)
                {
                    std::ostringstream oss;
                    oss << "```angelscript\n";
                    if (def->kind == analysis::LocalDefinitionKind::Field)
                    {
                        oss << "(field) ";
                    }
                    if (!def->typeName.empty())
                    {
                        oss << def->typeName << " ";
                    }
                    oss << def->name << "\n```";
                    std::string doc = analysis::ExtractDocComment(request.sourceCode, def->startLine);
                    if (!doc.empty())
                    {
                        oss << "\n\n" << doc;
                    }
                    return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
                }
            }
        }

        if (symbols.empty())
        {
            return std::nullopt;
        }

        RemoveDuplicateSymbols(symbols);

        std::ostringstream oss;
        oss << "```angelscript\n";

        for (size_t i = 0; i < symbols.size(); ++i)
        {
            if (i > 0)
            {
                oss << "\n";
            }
            oss << FormatDeclarationText(symbols[i]);
        }
        oss << "\n```";

        std::string doc = analysis::ExtractDocComment(request.sourceCode, symbols[0].startLine);
        if (!doc.empty())
        {
            oss << "\n\n" << doc;
        }

        return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
    }
}
