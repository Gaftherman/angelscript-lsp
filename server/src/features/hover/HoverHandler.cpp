#include "features/hover/HoverHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SignatureFormatter.h"
#include "analysis/DocComment.h"
#include "analysis/OverloadResolver.h"
#include "utils/Utils.h"
#include "utils/LspLogger.h"
#include "utils/Timer.h"
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include "parser/GrammarNames.h"

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief The documentation comment above a symbol's declaration, read out of its own file.
         *
         * `request.sourceCode` is the file being hovered over, and a symbol's `startLine` counts
         * lines in the file that *declares* it. Pairing the two showed whatever happened to be at
         * that line number here, which for a cross-file hover is an unrelated comment about
         * something else. Empty rather than wrong when the declaring file's text is not held: the
         * declaration may have been indexed and released, and a hover with no documentation is a
         * hover that simply says less.
         *
         * The same-file case still goes through `request.sourceCode` directly, without a lookup -
         * that is the common path and the URIs match by construction.
         */
        std::string OwnDocComment(const HoverRequest &request, const analysis::Symbol &symbol)
        {
            if (symbol.fileUri.empty() || symbol.fileUri == request.uri)
            {
                return analysis::ExtractDocComment(request.sourceCode, symbol.startLine);
            }

            if (!request.readDocument)
            {
                return "";
            }

            const std::string *declaringText = request.readDocument(symbol.fileUri);
            if (!declaringText)
            {
                return "";
            }

            return analysis::ExtractDocComment(*declaringText, symbol.startLine);
        }

        /**
         * @brief The comment on a symbol, or the one on the declaration it overrides.
         *
         * An implementation carries no comment of its own far more often than not - the interface
         * is where the contract is written, and repeating it on every implementer is exactly what
         * nobody does. Inherited only when the method has none itself, and only from an ancestor
         * of its own container, so an unrelated method with the same name elsewhere is never
         * consulted. The hierarchy walk returns the container first, so the loop skips it: it was
         * already asked, and it answered nothing.
         */
        std::string DocCommentForSymbol(const HoverRequest &request, const analysis::Symbol &symbol)
        {
            std::string own = OwnDocComment(request, symbol);
            if (!own.empty())
            {
                return own;
            }

            if (symbol.type != analysis::SymbolType::Function || symbol.containerName.empty())
            {
                return "";
            }

            for (const auto &ancestor : analysis::GetInheritedTypeHierarchy(symbol.containerName, request.symbolTable))
            {
                if (ancestor == symbol.containerName)
                {
                    continue;
                }

                const auto inherited = request.symbolTable.FindSymbolsPtr(ancestor + "::" + symbol.name);
                if (!inherited)
                {
                    continue;
                }

                for (const auto &candidate : *inherited)
                {
                    std::string doc = OwnDocComment(request, candidate);
                    if (!doc.empty())
                    {
                        return doc;
                    }
                }
            }

            return "";
        }

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
            if (sym.type != analysis::SymbolType::Variable && sym.type != analysis::SymbolType::Property)
            {
                return std::string(role) + sym.name;
            }
            if (!std::holds_alternative<analysis::VariableSignature>(sym.signature))
            {
                return std::string(role) + sym.name;
            }
            return std::string(role) + analysis::FormatVariableDeclaration(sym.GetVariable(), sym.name);
        }

        /** @brief Renders the single hover line that describes a symbol of any kind. */
        std::string FormatDeclarationText(const analysis::Symbol &sym, const analysis::SymbolTable *symbolTable = nullptr)
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
            {
                bool isProperty = false;
                if (!sym.containerName.empty())
                {
                    if (symbolTable)
                    {
                        auto containerSyms = symbolTable->FindSymbols(sym.containerName);
                        for (const auto &cs : containerSyms)
                        {
                            if (cs.type == analysis::SymbolType::Class || cs.type == analysis::SymbolType::Interface || cs.type == analysis::SymbolType::Enum)
                            {
                                isProperty = true;
                                break;
                            }
                        }
                    }
                    else
                    {
                        isProperty = true;
                    }
                }
                if (!isProperty && symbolTable)
                {
                    auto typeSyms = symbolTable->FindSymbols(sym.GetVariable().typeName);
                    for (const auto &ts : typeSyms)
                    {
                        if (ts.type == analysis::SymbolType::Enum)
                        {
                            isProperty = true;
                            break;
                        }
                    }
                }
                return FormatVariableSignature(sym, isProperty ? "(property) " : "(global variable) ");
            }
            case analysis::SymbolType::Typedef:
                return "typedef " + sym.GetTypedef().baseType + " " + sym.name;
            case analysis::SymbolType::Namespace:
                return "namespace " + sym.name;
            case analysis::SymbolType::Property:
                if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
                {
                    return FormatVariableSignature(sym, "(property) ");
                }
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

    namespace
    {
        /**
         * @brief The local definition whose own name spans this position.
         *
         * Null when the cursor is on a use rather than on a declaration, which is the ordinary case
         * and the one ResolveInScope answers. The ranges compared here are the name's, not the whole
         * declaration's, so a position inside one can only ever be that name.
         */
        const analysis::LocalDefinition *DefinitionAtPosition(const analysis::Scope *scope,
                                                              const lsp::Position &position)
        {
            for (const analysis::Scope *current = scope; current != nullptr; current = current->parent)
            {
                for (const analysis::LocalDefinition &def : current->definitions)
                {
                    const bool afterStart =
                        position.line > def.startLine ||
                        (position.line == def.startLine && position.character >= def.startCharacter);
                    const bool beforeEnd =
                        position.line < def.endLine ||
                        (position.line == def.endLine && position.character <= def.endCharacter);

                    if (afterStart && beforeEnd)
                        return &def;
                }
            }

            return nullptr;
        }
    }

    namespace
    {
        /**
         * @brief Answers a hover on an `#include` line with the file it actually resolves to.
         *
         * Reported as a want: the line says `#include "helper.as"` and gives no hint which of the
         * search directories won, or whether it resolved at all. The path is the answer.
         *
         * Text-based rather than tree-based on purpose. The grammar produces one opaque
         * `preproc_directive` node for the whole line, so there is nothing inside it to locate a
         * cursor against. This finds the quotes on the cursor's line and answers only when the
         * cursor is on the directive.
         *
         * @return A hover, or nullopt when the line is not an `#include` this can answer.
         */
        std::optional<lsp::Hover> HoverIncludeDirective(const HoverRequest &request)
        {
            const auto lineSpan = [&request]() -> std::pair<size_t, size_t>
            {
                size_t start = 0;
                for (uint32_t current = 0; current < request.position.line; ++current)
                {
                    const size_t nextBreak = request.sourceCode.find('\n', start);
                    if (nextBreak == std::string::npos)
                        return { std::string::npos, std::string::npos };
                    start = nextBreak + 1;
                }
                const size_t end = request.sourceCode.find('\n', start);
                return { start, end == std::string::npos ? request.sourceCode.size() : end };
            }();

            if (lineSpan.first == std::string::npos)
            {
                return std::nullopt;
            }

            const std::string_view line(request.sourceCode.data() + lineSpan.first,
                                        lineSpan.second - lineSpan.first);

            const size_t hash = line.find_first_not_of(" \t");
            if (hash == std::string_view::npos || line[hash] != '#')
            {
                return std::nullopt;
            }

            // No whitespace tolerated between the two, because the compiler tolerates none:
            // `# include "helper.as"` is not a directive at all and has its own diagnostic. See
            // PreprocessorRegions.h.
            if (line.compare(hash + 1, 7, "include") != 0)
            {
                return std::nullopt;
            }

            const size_t openQuote = line.find('"', hash + 8);
            if (openQuote == std::string_view::npos)
            {
                return std::nullopt;
            }
            const size_t closeQuote = line.find('"', openQuote + 1);
            if (closeQuote == std::string_view::npos)
            {
                return std::nullopt;
            }

            // Anywhere on the directive answers, not only on the quoted text: a user pointing at
            // `#include` is asking the same question as one pointing at the filename.
            const auto character = static_cast<size_t>(request.position.character);
            if (character < hash || character > closeQuote)
            {
                return std::nullopt;
            }

            const std::string rawPath(line.substr(openQuote + 1, closeQuote - openQuote - 1));

            std::string markdown = "```angelscript\n#include \"" + rawPath + "\"\n```";

            if (request.resolveInclude)
            {
                const std::string resolved = request.resolveInclude(rawPath);
                markdown += resolved.empty()
                    ? "\n\nDoes not resolve to a file. Checked this file's own directory, then each "
                      "`angelscript.searchDirectories` entry in order."
                    : "\n\n" + resolved;
            }

            lsp::Range range{};
            range.start.line = request.position.line;
            range.start.character = static_cast<lsp::uint>(hash);
            range.end.line = request.position.line;
            range.end.character = static_cast<lsp::uint>(closeQuote + 1);

            return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                                   std::move(markdown) },
                               range };
        }

        /**
         * @brief If node represents the callee in a call_expression, extracts argument types
         * and resolves the best matching candidate from the overload set.
         */
        std::optional<analysis::Symbol> ResolveCallOverload(
            TSNode node,
            const std::vector<analysis::Symbol> &candidates,
            const HoverRequest &request,
            const analysis::Scope *scope)
        {
            if (candidates.size() <= 1)
            {
                return std::nullopt;
            }

            TSNode callNode{};
            TSNode parent = ts_node_parent(node);
            if (!ts_node_is_null(parent))
            {
                std::string_view pType = ts_node_type(parent);
                if (pType == "call_expression")
                {
                    TSNode fn = parser::GetChildByField(parent, parser::fields::Function);
                    if (!ts_node_is_null(fn) && (ts_node_eq(fn, node) || ts_node_start_byte(fn) == ts_node_start_byte(node)))
                    {
                        callNode = parent;
                    }
                }
                else if (pType == "member_expression" || pType == "scoped_identifier")
                {
                    TSNode grandParent = ts_node_parent(parent);
                    if (!ts_node_is_null(grandParent) && std::string_view(ts_node_type(grandParent)) == "call_expression")
                    {
                        TSNode fn = parser::GetChildByField(grandParent, parser::fields::Function);
                        if (!ts_node_is_null(fn) && (ts_node_eq(fn, parent) || ts_node_start_byte(fn) == ts_node_start_byte(parent)))
                        {
                            callNode = grandParent;
                        }
                    }
                }
            }

            if (ts_node_is_null(callNode))
            {
                return std::nullopt;
            }

            TSNode argListNode = parser::GetChildByField(callNode, parser::fields::Arguments);
            if (ts_node_is_null(argListNode))
            {
                for (uint32_t i = 0; i < ts_node_child_count(callNode); ++i)
                {
                    TSNode ch = ts_node_child(callNode, i);
                    if (std::string_view(ts_node_type(ch)) == "argument_list")
                    {
                        argListNode = ch;
                        break;
                    }
                }
            }

            std::vector<std::string> argTypes;
            if (!ts_node_is_null(argListNode))
            {
                uint32_t count = ts_node_child_count(argListNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode ch = ts_node_child(argListNode, i);
                    std::string_view ct = ts_node_type(ch);
                    if (ct == "(" || ct == ")" || ct == "," || ct == "comment" || ct == ":")
                    {
                        continue;
                    }
                    const char *fieldName = ts_node_field_name_for_child(argListNode, i);
                    if (fieldName && std::string_view(fieldName) == "arg_name")
                    {
                        continue;
                    }
                    std::string aType = analysis::ResolveExpressionType(
                        ch, scope, request.symbolTable, request.sourceCode, request.uri);
                    argTypes.push_back(std::move(aType));
                }
            }

            auto match = analysis::ResolveBestOverload(candidates, argTypes, request.symbolTable);
            if (match.bestCandidate != nullptr)
            {
                return *match.bestCandidate;
            }

            // Fallback: when no candidate was strictly viable (e.g. argument type mismatch or syntax error),
            // score candidates by arity compatibility and parameter count match so the intended overload is prioritized.
            const analysis::Symbol *bestFallback = nullptr;
            int bestFallbackScore = -10000;
            const uint32_t argCount = static_cast<uint32_t>(argTypes.size());

            for (const auto &sym : candidates)
            {
                if (sym.type != analysis::SymbolType::Function || !std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                {
                    continue;
                }

                const auto &sig = sym.GetFunction();
                uint32_t requiredParams = 0;
                uint32_t maxParams = 0;
                bool isVariadic = false;

                for (const auto &param : sig.parameters)
                {
                    if (param.rawText.find("...") != std::string::npos)
                    {
                        isVariadic = true;
                        continue;
                    }
                    ++maxParams;
                    if (param.defaultValue.empty())
                    {
                        ++requiredParams;
                    }
                }

                int score = 0;
                bool arityMatches = (argCount >= requiredParams) && (isVariadic || argCount <= maxParams);
                if (arityMatches)
                {
                    score += 100;
                    if (argCount == sig.parameters.size())
                    {
                        score += 50;
                    }
                }
                else
                {
                    int diff = std::abs(static_cast<int>(argCount) - static_cast<int>(sig.parameters.size()));
                    score -= diff * 20;
                }

                for (size_t i = 0; i < argTypes.size() && i < sig.parameters.size(); ++i)
                {
                    if (!argTypes[i].empty())
                    {
                        int pScore = analysis::ScoreArgumentMatch(argTypes[i], sig.parameters[i], request.symbolTable);
                        if (pScore < 999)
                        {
                            score += 10;
                        }
                    }
                }

                if (score > bestFallbackScore)
                {
                    bestFallbackScore = score;
                    bestFallback = &sym;
                }
            }

            if (bestFallback != nullptr)
            {
                return *bestFallback;
            }

            return std::nullopt;
        }

        struct HoverProfiler
        {
            utils::HighResTimer totalTimer;
            angel_lsp::utils::LspLogger *m_logger = nullptr;
            uint32_t line = 0;
            uint32_t character = 0;
            double nodeMs = 0.0;
            double symMs = 0.0;
            double fmtMs = 0.0;
            bool emitted = false;

            void Emit()
            {
                if (!emitted && m_logger)
                {
                    emitted = true;
                    double totalMs = totalTimer.ElapsedMs();
                    m_logger->LogInfo(fmt::format(
                        "[Hover Profile] Total: {:.2f} ms (NodeLookup: {:.2f} ms, SymbolResolve: {:.2f} ms, Formatting: {:.2f} ms) at {}:{}",
                        totalMs, nodeMs, symMs, fmtMs, line, character));
                }
            }

            ~HoverProfiler()
            {
                Emit();
            }
        };
    }

    std::optional<lsp::Hover> GetHover(const HoverRequest &request)
    {
        if (!request.tree || request.sourceCode.empty())
        {
            return std::nullopt;
        }

        HoverProfiler profiler{ {}, request.logger, request.position.line, request.position.character };

        // Before the tree is consulted at all. A directive is not part of the AST - the grammar
        // gives the whole line one `preproc_directive` node with no structure inside it - so there
        // is no node here to hover and the answer has to come from the text.
        if (auto includeHover = HoverIncludeDirective(request))
        {
            return includeHover;
        }

        TSNode rootNode = ts_tree_root_node(request.tree);
        TSNode node{};
        std::string nodeText;
        lsp::Range range{};

        {
            utils::HighResTimer nodeTimer;
            if (!ExtractHoverNode(rootNode, request.sourceCode, request.position.line, request.position.character, node, nodeText, range))
            {
                profiler.nodeMs = nodeTimer.ElapsedMs();
                return std::nullopt;
            }
            profiler.nodeMs = nodeTimer.ElapsedMs();
        }

        // 1. Primitive type check
        if (analysis::IsPrimitiveTypeName(nodeText))
        {
            utils::HighResTimer fmtTimer;
            std::string md = "```angelscript\n(primitive type) " + nodeText + "\n```";
            profiler.fmtMs += fmtTimer.ElapsedMs();
            return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), md }, range };
        }

        // Check if cursor is in a virtual mixin document
        bool isVirtualDoc = request.uri.starts_with("angelscript-virtual:") || request.uri.starts_with("angelscript-virtual://");
        std::string virtualHostClass;
        std::string virtualMixinName;
        std::optional<analysis::Symbol> virtualMixinSym;

        if (isVirtualDoc)
        {
            virtualHostClass = analysis::SymbolTable::ExtractVirtualHostClass(request.uri);
            virtualMixinName = analysis::SymbolTable::ExtractVirtualMixinName(request.uri);

            auto candidates = request.symbolTable.FindSymbols(virtualMixinName);
            for (const auto &cand : candidates)
            {
                if (cand.type == analysis::SymbolType::Class)
                {
                    virtualMixinSym = cand;
                    break;
                }
            }
            if (!virtualMixinSym.has_value())
            {
                std::string shortName = virtualMixinName;
                auto lastScope = shortName.rfind("::");
                if (lastScope != std::string::npos)
                {
                    shortName = shortName.substr(lastScope + 2);
                }
                auto shortCandidates = request.symbolTable.FindTypeSymbolsByShortName(shortName);
                for (const auto &cand : shortCandidates)
                {
                    if (cand.type == analysis::SymbolType::Class)
                    {
                        virtualMixinSym = cand;
                        break;
                    }
                }
            }
        }

        auto rootScope = (isVirtualDoc && virtualMixinSym.has_value())
            ? request.scopeIndex.GetRoot(virtualMixinSym->fileUri)
            : request.scopeIndex.GetRoot(request.uri);

        uint32_t queryLine = (isVirtualDoc && virtualMixinSym.has_value())
            ? analysis::SymbolTable::VirtualToPhysicalLine(request.position.line, virtualMixinSym->startLine)
            : request.position.line;

        if (nodeText == "this")
        {
            std::string className;
            if (isVirtualDoc && !virtualHostClass.empty())
            {
                className = virtualHostClass;
            }
            else
            {
                auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
                for (const auto &c : containers)
                {
                    if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
                    {
                        className = c.name;
                        break;
                    }
                }
            }

            if (!className.empty())
            {
                utils::HighResTimer fmtTimer;
                std::string md = fmt::format("```angelscript\n{} {}\n```", className, nodeText);
                profiler.fmtMs += fmtTimer.ElapsedMs();
                return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), md }, range };
            }
        }

        TSNode parent = ts_node_parent(node);

        // Check if cursor node is the member child of a member_expression (e.g. "prop" in "obj.prop")
        bool isMemberChildOfExpression = false;
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
        {
            TSNode memNode = parser::GetChildByField(parent, parser::fields::Member);
            if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node)))
            {
                isMemberChildOfExpression = true;
            }
        }

        auto resolveMemberAccess = [&]() -> std::optional<lsp::Hover>
        {
            utils::HighResTimer symTimer;
            if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
            {
                profiler.symMs += symTimer.ElapsedMs();
                return std::nullopt;
            }

            TSNode objectNode = parser::GetChildByField(parent, parser::fields::Object);
            if (ts_node_is_null(objectNode))
            {
                profiler.symMs += symTimer.ElapsedMs();
                return std::nullopt;
            }

            const analysis::Scope *scope = rootScope ? FindInnermostScope(rootScope.get(), queryLine, request.position.character) : nullptr;
            std::string receiverTypeName = analysis::ResolveReceiverType(
                objectNode, request.sourceCode, request.symbolTable, scope, virtualHostClass, request.uri);
            if (!receiverTypeName.empty() && request.config && !request.config->types.arrayTypeName.empty())
            {
                receiverTypeName = analysis::MemberOwnerType(receiverTypeName, request.config->types.arrayTypeName);
            }

            if (!receiverTypeName.empty())
            {
                if (receiverTypeName.find("::") == std::string::npos && !request.symbolTable.HasSymbol(receiverTypeName))
                {
                    auto shortMatches = request.symbolTable.FindTypeSymbolsByShortName(receiverTypeName);
                    for (const auto &sym : shortMatches)
                    {
                        if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface)
                        {
                            receiverTypeName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                            break;
                        }
                    }
                }

                std::vector<analysis::Symbol> memberSymbols;
                auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, request.symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    std::string qualifiedMember = typeName + "::" + nodeText;
                    auto found = request.symbolTable.FindSymbols(qualifiedMember);
                    for (const auto &sym : found)
                    {
                        if (sym.type == analysis::SymbolType::Function)
                        {
                            bool overriddenLower = std::any_of(memberSymbols.begin(), memberSymbols.end(),
                                [&](const analysis::Symbol &kept) {
                                    return analysis::HasSameParameterList(kept, sym);
                                });
                            if (!overriddenLower)
                            {
                                memberSymbols.push_back(sym);
                            }
                        }
                        else
                        {
                            memberSymbols.push_back(sym);
                        }
                    }
                }

                // `e.Health` where the class declares `get_Health`/`set_Health` and nothing called
                // `Health`. The compiler derives the property from the accessors - measured, it
                // accepts both the read and the write - so hovering the one spelling that compiles
                // has to find something, or the editor denies what the build accepts.
                std::string accessorPropertyType;
                if (memberSymbols.empty())
                {
                    const int accessorMode =
                        request.config ? request.config->engine.propertyAccessorMode : 2;

                    // Modes 0 and 1 leave script-defined accessors out of the language, so there is
                    // no property to describe.
                    if (accessorMode >= 2)
                    {
                        memberSymbols = analysis::FindPropertyAccessors(
                            receiverTypeName, nodeText, request.symbolTable, accessorMode == 3);
                        accessorPropertyType = analysis::PropertyTypeFromAccessors(memberSymbols);
                    }
                }

                if (!memberSymbols.empty())
                {
                    if (auto best = ResolveCallOverload(node, memberSymbols, request, scope))
                    {
                        auto it = std::find_if(memberSymbols.begin(), memberSymbols.end(), [&](const analysis::Symbol &s) {
                            return s.name == best->name && analysis::HasSameParameterList(s, *best);
                        });
                        if (it != memberSymbols.end())
                        {
                            std::rotate(memberSymbols.begin(), it, it + 1);
                        }
                    }

                    RemoveDuplicateSymbols(memberSymbols);
                    profiler.symMs += symTimer.ElapsedMs();

                    utils::HighResTimer fmtTimer;
                    std::ostringstream oss;
                    oss << "```angelscript\n";

                    // Named first, because the accessors below are the implementation of it and the
                    // reader asked about the property.
                    if (!accessorPropertyType.empty())
                    {
                        oss << "(property) " << accessorPropertyType << " " << nodeText << "\n";
                    }
                    for (size_t i = 0; i < memberSymbols.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << "\n";
                        }
                        oss << FormatDeclarationText(memberSymbols[i]);
                    }
                    oss << "\n```";

                    std::vector<std::string> docs;
                    for (const auto &sym : memberSymbols)
                    {
                        std::string d = DocCommentForSymbol(request, sym);
                        if (!d.empty() && std::find(docs.begin(), docs.end(), d) == docs.end())
                        {
                            docs.push_back(std::move(d));
                        }
                    }
                    for (const auto &d : docs)
                    {
                        oss << "\n\n" << d;
                    }

                    profiler.fmtMs += fmtTimer.ElapsedMs();
                    return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
                }
            }

            profiler.symMs += symTimer.ElapsedMs();
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
            utils::HighResTimer symTimer;
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), queryLine, request.position.character);
            if (scope)
            {
                // The cursor may be *on* a declaration rather than on a use of one, and those need
                // different answers the moment a name is declared twice - which is the state a
                // document is in while the second one is being typed. ResolveInScope answers by
                // name and returns the first match in the chain, so hovering the `float count` a
                // user has just finished typing described the `int count` three lines above it.
                lsp::Position queryPos{ queryLine, request.position.character };
                const analysis::LocalDefinition *def = DefinitionAtPosition(scope, queryPos);
                if (!def)
                    def = analysis::ResolveInScope(scope, nodeText);
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
                                TSNode typeNode = parser::GetChildByField(cur, parser::fields::ParamType);
                                if (ts_node_is_null(typeNode))
                                {
                                    typeNode = parser::GetChildByField(cur, parser::fields::VarType);
                                }
                                if (ts_node_is_null(typeNode))
                                {
                                    typeNode = parser::GetChildByField(cur, parser::fields::Type);
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

                    profiler.symMs += symTimer.ElapsedMs();
                    utils::HighResTimer fmtTimer;
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
                            if (!def->defaultValue.empty())
                            {
                                oss << " = " << def->defaultValue;
                            }
                        }
                        break;
                    }
                    case analysis::LocalDefinitionKind::Variable:
                    {
                        const analysis::Scope *declaringScope = FindScopeDeclaringDefinition(rootScope.get(), *def);
                        bool isInsideFunction = false;
                        for (const analysis::Scope *s = declaringScope; s != nullptr; s = s->parent)
                        {
                            if (s->isFunctionScope)
                            {
                                isInsideFunction = true;
                                break;
                            }
                        }

                        if (!isInsideFunction)
                        {
                            const analysis::Symbol *globalSym = nullptr;
                            auto candidates = analysis::FindSymbolsInScope(def->name, node, request.sourceCode, request.symbolTable);
                            for (const auto &cand : candidates)
                            {
                                if (cand.type == analysis::SymbolType::Variable && cand.fileUri == request.uri)
                                {
                                    globalSym = &cand;
                                    break;
                                }
                            }
                            if (!globalSym && !candidates.empty())
                            {
                                for (const auto &cand : candidates)
                                {
                                    if (cand.type == analysis::SymbolType::Variable)
                                    {
                                        globalSym = &cand;
                                        break;
                                    }
                                }
                            }
                            if (!globalSym)
                            {
                                auto exactCandidates = request.symbolTable.FindSymbols(def->name);
                                for (const auto &cand : exactCandidates)
                                {
                                    if (cand.type == analysis::SymbolType::Variable && cand.fileUri == request.uri)
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
                                if (!def->defaultValue.empty())
                                {
                                    oss << " = " << def->defaultValue;
                                }
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
                            if (!def->defaultValue.empty())
                            {
                                oss << " = " << def->defaultValue;
                            }
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

                    profiler.fmtMs += fmtTimer.ElapsedMs();
                    return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
                }
            }
            profiler.symMs += symTimer.ElapsedMs();
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
        utils::HighResTimer symTimer;
        std::vector<analysis::Symbol> symbols;
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
        if (symbols.empty())
        {
            symbols = analysis::FindSymbolsInScope(nodeText, node, request.sourceCode, request.symbolTable);
        }

        // Also look up methods in enclosing class hierarchy if inside a class/interface
        auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
        for (const auto &c : containers)
        {
            if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
            {
                auto hierarchy = analysis::GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName, request.symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    auto found = request.symbolTable.FindSymbols(typeName + "::" + nodeText);
                    for (const auto &sym : found)
                    {
                        if (sym.type == analysis::SymbolType::Function)
                        {
                            bool overriddenLower = std::any_of(symbols.begin(), symbols.end(),
                                [&](const analysis::Symbol &kept) {
                                    return analysis::HasSameParameterList(kept, sym);
                                });
                            if (!overriddenLower)
                            {
                                symbols.push_back(sym);
                            }
                        }
                    }
                }
                break;
            }
        }

        std::string accessorPropertyType;
        if (isVirtualDoc && !virtualHostClass.empty())
        {
            std::vector<analysis::Symbol> hostSymbols;
            auto hierarchy = analysis::GetInheritedTypeHierarchy(virtualHostClass, request.symbolTable);
            for (const auto &typeName : hierarchy)
            {
                auto found = request.symbolTable.FindSymbols(typeName + "::" + nodeText);
                for (const auto &sym : found)
                {
                    if (sym.type == analysis::SymbolType::Function)
                    {
                        bool overriddenLower = std::any_of(hostSymbols.begin(), hostSymbols.end(),
                            [&](const analysis::Symbol &kept) {
                                return analysis::HasSameParameterList(kept, sym);
                            });
                        if (!overriddenLower)
                        {
                            hostSymbols.push_back(sym);
                        }
                    }
                    else if (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property)
                    {
                        hostSymbols.push_back(sym);
                    }
                }
            }

            if (hostSymbols.empty())
            {
                for (const auto &typeName : hierarchy)
                {
                    auto accessors = analysis::FindPropertyAccessors(typeName, nodeText, request.symbolTable, false);
                    if (!accessors.empty())
                    {
                        hostSymbols = std::move(accessors);
                        accessorPropertyType = analysis::PropertyTypeFromAccessors(hostSymbols);
                        break;
                    }
                }
            }

            if (!hostSymbols.empty())
            {
                // Container members from mixin take precedence over host class overloads;
                // host class members take precedence over global identifiers.
                std::vector<analysis::Symbol> containerSymbols;
                for (const auto &s : symbols)
                {
                    if (!s.containerName.empty() || s.fileUri == request.uri)
                    {
                        containerSymbols.push_back(s);
                    }
                }
                symbols = std::move(containerSymbols);

                for (auto &hs : hostSymbols)
                {
                    bool present = std::any_of(symbols.begin(), symbols.end(), [&](const analysis::Symbol &s) {
                        return s.name == hs.name && analysis::HasSameParameterList(s, hs);
                    });
                    if (!present)
                    {
                        symbols.push_back(std::move(hs));
                    }
                }
            }
        }

        // Fallback to local scope definition (e.g. Field or non-function variable) if not found in SymbolTable
        if (symbols.empty() && rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), queryLine, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def)
                {
                    profiler.symMs += symTimer.ElapsedMs();
                    utils::HighResTimer fmtTimer;
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
                    profiler.fmtMs += fmtTimer.ElapsedMs();
                    return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
                }
            }
        }

        if (symbols.empty() && accessorPropertyType.empty())
        {
            const int accessorMode =
                request.config ? request.config->engine.propertyAccessorMode : 2;

            if (accessorMode >= 2)
            {
                symbols = analysis::FindGlobalPropertyAccessors(nodeText, request.symbolTable, accessorMode == 3);
                accessorPropertyType = analysis::PropertyTypeFromAccessors(symbols);
            }
        }

        if (symbols.empty())
        {
            profiler.symMs += symTimer.ElapsedMs();
            return std::nullopt;
        }

        const analysis::Scope *scope = rootScope ? FindInnermostScope(rootScope.get(), queryLine, request.position.character) : nullptr;
        if (auto best = ResolveCallOverload(node, symbols, request, scope))
        {
            auto it = std::find_if(symbols.begin(), symbols.end(), [&](const analysis::Symbol &s) {
                return s.name == best->name && analysis::HasSameParameterList(s, *best);
            });
            if (it != symbols.end())
            {
                std::rotate(symbols.begin(), it, it + 1);
            }
        }

        RemoveDuplicateSymbols(symbols);
        profiler.symMs += symTimer.ElapsedMs();

        utils::HighResTimer fmtTimer;
        std::ostringstream oss;
        oss << "```angelscript\n";

        if (!accessorPropertyType.empty())
        {
            oss << "(property) " << accessorPropertyType << " " << nodeText << "\n";
        }

        for (size_t i = 0; i < symbols.size(); ++i)
        {
            if (i > 0)
            {
                oss << "\n";
            }
            oss << FormatDeclarationText(symbols[i], &request.symbolTable);
        }
        oss << "\n```";

        std::vector<std::string> docs;
        for (const auto &sym : symbols)
        {
            std::string d = DocCommentForSymbol(request, sym);
            if (!d.empty() && std::find(docs.begin(), docs.end(), d) == docs.end())
            {
                docs.push_back(std::move(d));
            }
        }
        for (const auto &d : docs)
        {
            oss << "\n\n" << d;
        }

        profiler.fmtMs += fmtTimer.ElapsedMs();
        return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
    }
}
