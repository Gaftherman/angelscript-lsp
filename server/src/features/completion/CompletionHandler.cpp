#include "features/completion/CompletionHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/DocComment.h"
#include "analysis/SignatureFormatter.h"
#include "utils/PositionEncoding.h"
#include <unordered_set>
#include <sstream>
#include <regex>

namespace angel_lsp::features
{
    namespace
    {

        // `CanonicalizeArrayType` lived here, and only here, which is why `int[] a; a.length()`
        // completed correctly and had neither hover nor call checking - the other two passes had
        // no such function to call. It is now `analysis::CanonicalizeArrayType`, used by all three.
        using analysis::CanonicalizeArrayType;

        /**
         * @brief True when a member is its class's constructor or destructor.
         *
         * Neither can be called on an instance. AngelScript has no syntax for it: the real compiler
         * rejects `m.Matrix()` with "No matching symbol 'Matrix'", and `Matrix m.Matrix();` does not
         * parse at all. Offering them after `m.` invites the user to write something that cannot
         * compile.
         *
         * Detected by name rather than by a flag because that is the convention the rest of the
         * analyzer uses - a constructor is stored as an ordinary Function whose name matches its
         * container (CallChecker looks up `Class::Class` the same way). The container may arrive
         * qualified, so the comparison is against its last `::` segment.
         */
        bool IsConstructorOrDestructor(const analysis::Symbol &sym, const std::string &typeName)
        {
            if (sym.type != analysis::SymbolType::Function)
            {
                return false;
            }

            const size_t at = typeName.rfind("::");
            const std::string_view shortName =
                at == std::string::npos ? std::string_view(typeName)
                                        : std::string_view(typeName).substr(at + 2);

            if (sym.name == shortName)
            {
                return true;
            }

            return !sym.name.empty() && sym.name.front() == '~' &&
                   std::string_view(sym.name).substr(1) == shortName;
        }

        std::string FormatMethodDetail(const analysis::Symbol &sym, const std::vector<std::string> &templateArgs)
        {
            if (sym.type != analysis::SymbolType::Function)
            {
                return "";
            }
            analysis::Symbol substitutedSym = sym;
            auto fn = sym.GetFunction();
            if (!templateArgs.empty())
            {
                fn.returnType = analysis::SubstituteTypeParam(fn.returnType, "T", templateArgs[0]);
                for (auto &param : fn.parameters)
                {
                    param.typeName = analysis::SubstituteTypeParam(param.typeName, "T", templateArgs[0]);
                    param.baseTypeName = analysis::SubstituteTypeParam(param.baseTypeName, "T", templateArgs[0]);
                }
            }
            substitutedSym.signature = fn;
            return analysis::FormatFunctionDeclaration(substitutedSym, false);
        }

        /**
         * @brief Recursively collects the class and interface inheritance hierarchy for a type.
         * @param symbolTable The symbol table to look up class and interface definitions.
         * @param initialTypeName The starting type name.
         * @return Vector of type names in the hierarchy including initialTypeName and its transitive bases.
         */
        std::vector<std::string> GetInheritedTypeHierarchy(const analysis::SymbolTable &symbolTable, const std::string &initialTypeName)
        {
            std::vector<std::string> hierarchy;
            std::unordered_set<std::string> visited;

            auto visitType = [&](auto &self, const std::string &currentTypeName) -> void
            {
                if (currentTypeName.empty() || visited.find(currentTypeName) != visited.end())
                {
                    return;
                }
                visited.insert(currentTypeName);
                hierarchy.push_back(currentTypeName);

                auto syms = symbolTable.FindSymbols(currentTypeName);
                for (const auto &sym : syms)
                {
                    if (sym.type == analysis::SymbolType::Class)
                    {
                        const auto &classSig = sym.GetClass();
                        for (const auto &baseName : classSig.bases)
                        {
                            self(self, baseName);
                        }
                    }
                    else if (sym.type == analysis::SymbolType::Interface)
                    {
                        const auto &ifaceSig = sym.GetInterface();
                        for (const auto &baseName : ifaceSig.inheritedInterfaces)
                        {
                            self(self, baseName);
                        }
                    }
                }
            };

            visitType(visitType, initialTypeName);
            return hierarchy;
        }

        std::string GetLinePrefix(const std::string &sourceCode, uint32_t line, uint32_t character)
        {
            size_t currentLine = 0;
            size_t lineStart = 0;

            for (size_t i = 0; i < sourceCode.size(); ++i)
            {
                if (currentLine == line)
                {
                    lineStart = i;
                    break;
                }
                if (sourceCode[i] == '\n')
                {
                    currentLine++;
                }
            }

            if (currentLine != line)
            {
                return "";
            }

            size_t lineEnd = sourceCode.find('\n', lineStart);
            if (lineEnd == std::string::npos)
            {
                lineEnd = sourceCode.size();
            }

            size_t prefixLen = std::min(static_cast<size_t>(character), lineEnd - lineStart);
            return sourceCode.substr(lineStart, prefixLen);
        }

        void AddItemIfNew(std::vector<lsp::CompletionItem> &items,
                          std::unordered_set<std::string> &seenLabels,
                          const std::string &label,
                          lsp::CompletionItemKind kind,
                          const std::string &detail = "",
                          const std::string &doc = "",
                          const std::string &resolveKey = "",
                          const std::string &snippet = "")
        {
            if (label.empty() || seenLabels.contains(label))
            {
                return;
            }
            seenLabels.insert(label);

            lsp::CompletionItem item;
            item.label = label;
            item.kind = lsp::CompletionItemKindEnum(kind);
            if (!snippet.empty())
            {
                item.insertText = snippet;
                item.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
            }
            if (!detail.empty())
            {
                item.detail = detail;
            }
            if (!doc.empty())
            {
                item.documentation = lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), doc };
            }
            if (!resolveKey.empty())
            {
                // The qualified name is all the identity a resolve needs: it finds the symbol,
                // and the symbol knows which file - and which line - its documentation lives above.
                item.data = lsp::LSPAny(std::string(resolveKey));
            }
            items.push_back(std::move(item));
        }

        /** @brief The primitive type names, for the contexts where only a type may be written. */
        const std::vector<std::string> &GetPrimitiveTypeNames()
        {
            static const std::vector<std::string> primitives = {
                "void", "bool", "int", "int8", "int16", "int32", "int64",
                "uint", "uint8", "uint16", "uint32", "uint64",
                "float", "double", "string", "array", "dictionary"
            };
            return primitives;
        }

        /** @brief True for the symbol kinds that may legally appear as a template argument. */
        bool IsTypeSymbol(const analysis::Symbol &sym)
        {
            switch (sym.type)
            {
            case analysis::SymbolType::Class:
            case analysis::SymbolType::Interface:
            case analysis::SymbolType::Enum:
            case analysis::SymbolType::Typedef:
            case analysis::SymbolType::Funcdef:
                return true;
            default:
                return false;
            }
        }

        /**
         * @brief The snippet a template class completes to, or empty when it is not one.
         *
         * `array` on its own is not a type - AngelScript has no default argument for `T` - so the
         * completion has to carry the brackets with it, and putting the cursor between them is the
         * whole point of the placeholder. The parameter names come from the declaration, so
         * `array<T>` and a host's `map<K,V>` each read back the names their author chose.
         */
        std::string TemplateInsertSnippet(const analysis::Symbol &sym)
        {
            if (sym.type != analysis::SymbolType::Class ||
                !std::holds_alternative<analysis::ClassSignature>(sym.signature))
            {
                return {};
            }

            const auto &cls = std::get<analysis::ClassSignature>(sym.signature);
            if (!cls.isTemplate || cls.templateParams.empty())
            {
                return {};
            }

            std::string snippet = sym.name + "<";
            for (size_t i = 0; i < cls.templateParams.size(); ++i)
            {
                if (i > 0)
                {
                    snippet += ", ";
                }
                snippet += "${" + std::to_string(i + 1) + ":" + cls.templateParams[i] + "}";
            }
            snippet += ">$0";
            return snippet;
        }

        /** @brief Where in the source's lexical structure a byte offset falls. */
        enum class LexicalContext
        {
            Code,           ///< Anywhere a symbol may be written.
            Comment,        ///< Inside `//`, `/*` or a `///` doc comment.
            StringLiteral,  ///< Inside `"..."`, `'...'` or a `"""..."""` heredoc.
        };

        /**
         * @brief Classifies the byte at `offset` as code, comment or string.
         *
         * Scanned by hand rather than read off `request.tree`, because the tree cannot answer this
         * question in the state completion is asked in. A string being typed is unterminated and an
         * open block comment swallows the rest of the file; both arrive as an ERROR node rather
         * than as a string or a comment - and those are exactly the moments suppression is for.
         *
         * The rules are the default engine's, matching the scanners in FormattingHandler,
         * PreprocessorRegions and IncludeResolver: `"` and `'` each open a string that ends at the
         * closing quote or at the line break, since multiline strings are an engine property that
         * is off by default; `"""` opens a heredoc, which does span lines and processes no escape
         * sequences; and a block comment does not nest.
         */
        LexicalContext ContextAtOffset(std::string_view source, size_t offset)
        {
            const size_t end = std::min(offset, source.size());
            size_t i = 0;

            while (i < end)
            {
                const char c = source[i];

                if (c == '/' && i + 1 < source.size() && source[i + 1] == '/')
                {
                    const size_t close = source.find('\n', i + 2);
                    if (close == std::string_view::npos || close >= end)
                    {
                        return LexicalContext::Comment;
                    }
                    i = close + 1;
                    continue;
                }

                if (c == '/' && i + 1 < source.size() && source[i + 1] == '*')
                {
                    const size_t close = source.find("*/", i + 2);
                    if (close == std::string_view::npos || close + 2 > end)
                    {
                        return LexicalContext::Comment;
                    }
                    i = close + 2;
                    continue;
                }

                if (c == '"' && i + 2 < source.size() && source[i + 1] == '"' && source[i + 2] == '"')
                {
                    const size_t close = source.find("\"\"\"", i + 3);
                    if (close == std::string_view::npos || close + 3 > end)
                    {
                        return LexicalContext::StringLiteral;
                    }
                    i = close + 3;
                    continue;
                }

                if (c == '"' || c == '\'')
                {
                    size_t j = i + 1;
                    while (j < source.size() && source[j] != '\n' && source[j] != c)
                    {
                        // A backslash escapes the next character, but never the line break: the
                        // string still ends there.
                        if (source[j] == '\\' && j + 1 < source.size() && source[j + 1] != '\n')
                        {
                            ++j;
                        }
                        ++j;
                    }

                    // `j` is the closing quote, the line break that cut the string short, or the
                    // end of the file. The offset is inside the literal up to and including `j`;
                    // one past it is code again.
                    if (end <= j)
                    {
                        return LexicalContext::StringLiteral;
                    }
                    i = j + 1;
                    continue;
                }

                ++i;
            }

            return LexicalContext::Code;
        }

        /**
         * @brief True when the prefix ends at the `:` that closes a `case` or `default` label.
         *
         * `:` is a completion trigger character because of `::`, so typing the colon of `case Red:`
         * asked for completion and was handed the whole global scope - every local, every global
         * and all 60 keywords - at a position where nothing at all may be written. A trailing `::`
         * is left alone: that is the qualifier case, which reads the same prefix and answers it.
         */
        bool IsAfterCaseLabelColon(const std::string &prefix)
        {
            if (prefix.empty() || prefix.back() != ':')
            {
                return false;
            }
            if (prefix.size() >= 2 && prefix[prefix.size() - 2] == ':')
            {
                return false;
            }

            // Anchored on a word boundary rather than on the line start, so a one-line
            // `switch (x) { case 1:` is recognised too. `case Some::Value:` keeps its `::`.
            static const std::regex caseLabelRegex(R"((^|[\s{};])(case\s+[^;]*|default\s*):$)");
            return std::regex_search(prefix, caseLabelRegex);
        }

        /**
         * @brief True when the cursor sits inside an unclosed `Name<...>` argument list.
         *
         * Scanned right to left rather than matched with a regex, because the answer depends on
         * nesting: in `array<array<` the cursor is two levels deep, and a pattern that stopped at
         * the first `<` would have said the same thing about `a < b`. The `<` only counts when an
         * identifier character sits immediately before it, which is what separates the template
         * bracket from the comparison operator.
         */
        bool IsInsideTemplateArguments(std::string_view prefix)
        {
            int depth = 0;
            for (size_t i = prefix.size(); i-- > 0;)
            {
                const char c = prefix[i];
                if (c == ';' || c == '{' || c == '}' || c == '(' || c == ')')
                {
                    return false;
                }
                if (c == '>')
                {
                    ++depth;
                }
                else if (c == '<')
                {
                    if (depth > 0)
                    {
                        --depth;
                        continue;
                    }
                    if (i == 0)
                    {
                        return false;
                    }
                    const char before = prefix[i - 1];
                    return std::isalnum(static_cast<unsigned char>(before)) || before == '_';
                }
            }
            return false;
        }

        /**
         * @brief The template snippet for a name, looked up through the symbol table.
         *
         * The scope-chain pass below names a type without holding its declaration, and it runs
         * first - so it wins the de-duplication, and `array` reached the client as a bare name
         * however carefully the symbol-table pass was written. This is the bridge between the two.
         */
        std::string TemplateSnippetForName(const std::string &name, const analysis::SymbolTable &table)
        {
            if (const auto symbols = table.FindSymbolsPtr(name))
            {
                for (const auto &sym : *symbols)
                {
                    std::string snippet = TemplateInsertSnippet(sym);
                    if (!snippet.empty())
                    {
                        return snippet;
                    }
                }
            }
            return {};
        }

        const std::vector<std::string> &GetKeywords()
        {
            static const std::vector<std::string> keywords = {
                // Primitive Types
                "void", "bool", "int", "int8", "int16", "int32", "int64",
                "uint", "uint8", "uint16", "uint32", "uint64",
                "float", "double", "string", "auto",
                // Control Flow
                "if", "else", "for", "foreach", "while", "do",
                "switch", "case", "default", "break", "continue",
                "return", "try", "catch",
                // Declarations & Modifiers
                "class", "interface", "enum", "funcdef", "typedef",
                "namespace", "import", "from", "using", "mixin",
                "const", "final", "abstract", "override", "explicit",
                "private", "protected", "public", "shared", "external",
                "property", "delete", "in", "out", "inout", "cast",
                "null", "true", "false", "this", "super", "get", "set"
            };
            return keywords;
        }
    }

    std::vector<lsp::CompletionItem> GetCompletion(const CompletionRequest &request)
    {
        std::vector<lsp::CompletionItem> items;
        std::unordered_set<std::string> seenLabels;

        std::string prefix = GetLinePrefix(request.sourceCode, request.position.line, request.position.character);
        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        const analysis::Scope *innermostScope = nullptr;
        if (rootScope)
        {
            innermostScope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
        }

        // 0. Positions where nothing may be completed. A comment, a string literal and the colon
        //    of a `case` label are all places the language has no symbol for, and each of them
        //    fell through to the global fallback below and answered with the entire scope.
        const size_t cursorOffset =
            utils::LineStartOffset(request.sourceCode, request.position.line) + prefix.size();
        if (ContextAtOffset(request.sourceCode, cursorOffset) != LexicalContext::Code ||
            IsAfterCaseLabelColon(prefix))
        {
            return items;
        }

        // 1. Check for Scope Resolution Context: "Qualifier::" or "Qualifier::partial"
        static const std::regex scopeResolutionRegex(R"(([a-zA-Z_][a-zA-Z0-9_]*)::([a-zA-Z_][a-zA-Z0-9_]*)?$)");
        std::smatch scopeMatch;
        if (std::regex_search(prefix, scopeMatch, scopeResolutionRegex))
        {
            std::string qualifier = scopeMatch[1].str();

            // Collect all symbols under qualifier
            request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
            {
                for (const auto &sym : symList)
                {
                    if (sym.containerName == qualifier)
                    {
                        lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
                        std::string detail;
                        if (sym.type == analysis::SymbolType::Function)
                        {
                            kind = lsp::CompletionItemKind::Function;
                            const auto &fn = sym.GetFunction();
                            detail = fn.returnType + " " + sym.name + "(...)";
                        }
                        else if (sym.type == analysis::SymbolType::Variable)
                        {
                            kind = lsp::CompletionItemKind::Variable;
                            const auto &var = sym.GetVariable();
                            detail = var.typeName;
                        }
                        else if (sym.type == analysis::SymbolType::Class)
                        {
                            kind = lsp::CompletionItemKind::Class;
                        }
                        else if (sym.type == analysis::SymbolType::Enum)
                        {
                            kind = lsp::CompletionItemKind::Enum;
                        }

                        AddItemIfNew(items, seenLabels, sym.name, kind, detail, "", sym.qualifiedName);
                    }
                    else if (sym.type == analysis::SymbolType::Enum && sym.name == qualifier)
                    {
                        const auto &eSig = sym.GetEnum();
                        for (const auto &mem : eSig.members)
                        {
                            AddItemIfNew(items, seenLabels, mem.name, lsp::CompletionItemKind::EnumMember, qualifier + "::" + mem.name);
                        }
                    }
                }
            });

            return items;
        }

        // 1b. Template argument context: "array<" or "array<array<". Only a type may be written
        //     here, so offering the whole lexical scope - variables, functions, `while` - would be
        //     offering nothing but wrong answers. This runs after the `::` case above so that
        //     `array<Some::` still completes through the qualifier.
        if (IsInsideTemplateArguments(prefix))
        {
            request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
            {
                for (const auto &sym : symList)
                {
                    if (!sym.containerName.empty() || !IsTypeSymbol(sym))
                    {
                        continue;
                    }

                    lsp::CompletionItemKind kind = lsp::CompletionItemKind::Class;
                    switch (sym.type)
                    {
                    case analysis::SymbolType::Interface: kind = lsp::CompletionItemKind::Interface; break;
                    case analysis::SymbolType::Enum:      kind = lsp::CompletionItemKind::Enum; break;
                    case analysis::SymbolType::Typedef:
                    case analysis::SymbolType::Funcdef:   kind = lsp::CompletionItemKind::TypeParameter; break;
                    default: break;
                    }

                    const std::string snippet = request.snippetSupport ? TemplateInsertSnippet(sym) : std::string{};
                    AddItemIfNew(items, seenLabels, sym.name, kind, "", "", sym.qualifiedName, snippet);
                }
            });

            for (const auto &primitive : GetPrimitiveTypeNames())
            {
                AddItemIfNew(items, seenLabels, primitive, lsp::CompletionItemKind::Keyword);
            }

            return items;
        }

        // 2. Check for Member Access Context: "receiver." or "receiver->" (with optional partial identifier)
        static const std::regex memberAccessRegex(R"(([a-zA-Z_][a-zA-Z0-9_]*(?:\[[^\]]*\])*)(?:\.|\->)([a-zA-Z_][a-zA-Z0-9_]*)?$)");
        std::smatch memberMatch;
        if (std::regex_search(prefix, memberMatch, memberAccessRegex))
        {
            std::string receiverFull = memberMatch[1].str();
            size_t bracketPos = receiverFull.find('[');
            std::string receiverName = (bracketPos != std::string::npos) ? receiverFull.substr(0, bracketPos) : receiverFull;
            size_t indexCount = 0;
            for (char ch : receiverFull)
            {
                if (ch == '[') ++indexCount;
            }

            std::string rawTypeName;

            if (receiverName == "this")
            {
                request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.type == analysis::SymbolType::Class && sym.fileUri == request.uri)
                        {
                            if (request.position.line >= sym.startLine && request.position.line <= sym.endLine)
                            {
                                rawTypeName = sym.name;
                            }
                        }
                    }
                });
            }
            else if (innermostScope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(innermostScope, receiverName);
                if (def && !def->typeName.empty())
                {
                    rawTypeName = def->typeName;
                }
            }

            if (rawTypeName.empty())
            {
                // Check if receiverName is a global variable
                auto globSyms = request.symbolTable.FindSymbols(receiverName);
                for (const auto &sym : globSyms)
                {
                    if (sym.type == analysis::SymbolType::Variable)
                    {
                        const auto &var = sym.GetVariable();
                        if (!var.typeName.empty())
                        {
                            rawTypeName = var.typeName;
                            break;
                        }
                    }
                }
            }

            if (!rawTypeName.empty())
            {
                std::string arrayContainer = (request.config && !request.config->types.arrayTypeName.empty()) ? request.config->types.arrayTypeName : "array";
                std::string canonicalType = CanonicalizeArrayType(rawTypeName, arrayContainer);

                for (size_t idx = 0; idx < indexCount; ++idx)
                {
                    auto tmpl = analysis::ParseTemplateType(canonicalType);
                    if (!tmpl.templateArgs.empty())
                    {
                        canonicalType = tmpl.templateArgs[0];
                    }
                    else if (canonicalType.ends_with("[]"))
                    {
                        canonicalType = canonicalType.substr(0, canonicalType.size() - 2);
                    }
                }

                auto targetTemplate = analysis::ParseTemplateType(canonicalType);
                std::string baseContainer = targetTemplate.containerName;
                std::vector<std::string> templateArgs = targetTemplate.templateArgs;

                // Probes the version-cached container index rather than walking the whole workspace
                // table. This ran once per type in the inheritance chain, on every keystroke, over
                // a table the codebase's own comments size at fifty thousand symbols - so a member
                // completion on a class with three bases was four full-table scans per character.
                const auto ruleIndex = request.symbolTable.GetRuleIndex();

                auto addMembersForType = [&](const std::string &typeName)
                {
                    const auto &members = ruleIndex->Members(typeName);

                    for (const auto &key : members.memberKeys)
                    {
                        const auto symbols = request.symbolTable.FindSymbolsPtr(key);
                        if (!symbols)
                        {
                            continue;
                        }

                        for (const auto &sym : *symbols)
                        {
                            if (sym.containerName == typeName && !IsConstructorOrDestructor(sym, typeName))
                            {
                                lsp::CompletionItemKind kind = lsp::CompletionItemKind::Field;
                                std::string detail;
                                if (sym.type == analysis::SymbolType::Function)
                                {
                                    kind = lsp::CompletionItemKind::Method;
                                    detail = FormatMethodDetail(sym, templateArgs);
                                }
                                else if (sym.type == analysis::SymbolType::Variable)
                                {
                                    kind = lsp::CompletionItemKind::Field;
                                    const auto &var = sym.GetVariable();
                                    detail = var.typeName;
                                    if (!templateArgs.empty())
                                    {
                                        detail = analysis::SubstituteTypeParam(detail, "T", templateArgs[0]);
                                    }
                                }
                                else if (sym.type == analysis::SymbolType::Property)
                                {
                                    kind = lsp::CompletionItemKind::Property;
                                }

                                AddItemIfNew(items, seenLabels, sym.name, kind, detail, "", sym.qualifiedName);
                            }
                        }
                    }
                };

                auto hierarchy = GetInheritedTypeHierarchy(request.symbolTable, baseContainer);
                for (const auto &typeName : hierarchy)
                {
                    addMembersForType(typeName);
                }
            }

            return items;
        }

        // 3. Lexical & Global Completion

        // A. Local definitions in scope chain
        if (innermostScope)
        {
            for (const analysis::Scope *cur = innermostScope; cur != nullptr; cur = cur->parent)
            {
                for (const auto &def : cur->definitions)
                {
                    lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
                    bool isCallable = false;
                    std::string snippet;
                    if (def.kind == analysis::LocalDefinitionKind::Parameter)
                    {
                        kind = lsp::CompletionItemKind::Variable;
                    }
                    else if (def.kind == analysis::LocalDefinitionKind::Function ||
                             def.kind == analysis::LocalDefinitionKind::Method)
                    {
                        kind = lsp::CompletionItemKind::Function;
                        isCallable = true;
                    }
                    else if (def.kind == analysis::LocalDefinitionKind::Type)
                    {
                        kind = lsp::CompletionItemKind::Class;
                        if (request.snippetSupport)
                        {
                            snippet = TemplateSnippetForName(def.name, request.symbolTable);
                        }
                    }

                    // A resolve key only for the function-like definitions, which are the ones the
                    // symbol table also holds and can therefore be resolved to a doc comment. A
                    // local variable has no symbol-table entry to look up, and this path runs first
                    // - so without the key here a module-level function would reach the client with
                    // no identity at all and never resolve.
                    AddItemIfNew(items, seenLabels, def.name, kind, def.typeName, "",
                                 isCallable ? def.name : std::string{}, snippet);
                }
            }
        }

        // B. Enclosing class members (if cursor is inside a class method / body)
        std::string enclosingClassName;
        request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
        {
            for (const auto &sym : symbols)
            {
                if (sym.type == analysis::SymbolType::Class && sym.fileUri == request.uri)
                {
                    if (request.position.line >= sym.startLine && request.position.line <= sym.endLine)
                    {
                        enclosingClassName = sym.name;
                    }
                }
            }
        });

        if (!enclosingClassName.empty())
        {
            request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
            {
                for (const auto &sym : symList)
                {
                    if (sym.containerName == enclosingClassName)
                    {
                        lsp::CompletionItemKind kind = (sym.type == analysis::SymbolType::Function) ?
                            lsp::CompletionItemKind::Method : lsp::CompletionItemKind::Field;
                        AddItemIfNew(items, seenLabels, sym.name, kind);
                    }
                }
            });
        }

        // C. Global Symbols from SymbolTable
        request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
        {
            for (const auto &sym : symList)
            {
                if (sym.containerName.empty() && sym.type != analysis::SymbolType::CallReference)
                {
                    lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
                    std::string detail;
                    std::string snippet;
                    switch (sym.type)
                    {
                    case analysis::SymbolType::Function:
                        kind = lsp::CompletionItemKind::Function;
                        detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
                        break;
                    case analysis::SymbolType::Class:
                        kind = lsp::CompletionItemKind::Class;
                        // A template class completes to `array<T>`, not `array`: the bare name is
                        // not a type anywhere it could be written.
                        if (request.snippetSupport)
                        {
                            snippet = TemplateInsertSnippet(sym);
                        }
                        if (!snippet.empty())
                        {
                            detail = sym.name + "<" + std::get<analysis::ClassSignature>(sym.signature).templateParams.front() + ">";
                        }
                        break;
                    case analysis::SymbolType::Interface:
                        kind = lsp::CompletionItemKind::Interface;
                        break;
                    case analysis::SymbolType::Enum:
                        kind = lsp::CompletionItemKind::Enum;
                        break;
                    case analysis::SymbolType::Typedef:
                    case analysis::SymbolType::Funcdef:
                        kind = lsp::CompletionItemKind::TypeParameter;
                        break;
                    case analysis::SymbolType::Namespace:
                        kind = lsp::CompletionItemKind::Module;
                        break;
                    case analysis::SymbolType::Variable:
                        kind = lsp::CompletionItemKind::Variable;
                        detail = sym.GetVariable().typeName;
                        break;
                    case analysis::SymbolType::Property:
                        kind = lsp::CompletionItemKind::Property;
                        break;
                    default:
                        break;
                    }
                    AddItemIfNew(items, seenLabels, sym.name, kind, detail, "", sym.qualifiedName, snippet);
                }
            }
        });

        // D. AngelScript Keywords
        for (const auto &kw : GetKeywords())
        {
            AddItemIfNew(items, seenLabels, kw, lsp::CompletionItemKind::Keyword);
        }

        return items;
    }

    lsp::CompletionItem ResolveCompletionItem(const CompletionResolveRequest &request)
    {
        lsp::CompletionItem resolved = request.item;

        // Already answered, or nothing to answer with: a keyword item carries no identity, and an
        // item that arrived with documentation has nothing left to resolve.
        if (resolved.documentation.has_value() || !resolved.data.has_value() || !resolved.data->isString())
        {
            return resolved;
        }

        const std::string qualifiedName = resolved.data->string();
        if (qualifiedName.empty() || !request.readDocument)
        {
            return resolved;
        }

        for (const auto &symbol : request.symbolTable.FindSymbols(qualifiedName))
        {
            const std::string *text = request.readDocument(symbol.fileUri);
            if (!text)
            {
                // The declaring file is not one the server holds text for - a workspace file that
                // was indexed and released, say. Nothing to read the comment out of.
                continue;
            }

            const std::string documentation = analysis::ExtractDocComment(*text, symbol.startLine);
            if (documentation.empty())
            {
                continue;
            }

            resolved.documentation = lsp::MarkupContent{
                lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), documentation };
            break;
        }

        return resolved;
    }
}
