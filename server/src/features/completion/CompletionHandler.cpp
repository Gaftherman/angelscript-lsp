#include "features/completion/CompletionHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/DocComment.h"
#include "analysis/SignatureFormatter.h"
#include "utils/PositionEncoding.h"
#include <unordered_set>
#include <sstream>
#include <regex>
#include "utils/IncludeResolver.h"
#include <optional>
#include <filesystem>
#include "parser/Keywords.h"

#include <iterator>

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

        std::string FormatMethodDetail(const analysis::Symbol &sym,
                                       const analysis::TemplateBinding &binding,
                                       const std::vector<std::string> &templateArgs)
        {
            if (sym.type != analysis::SymbolType::Function)
            {
                return "";
            }
            analysis::Symbol substitutedSym = sym;
            auto fn = sym.GetFunction();
            if (binding.usable)
            {
                for (size_t i = 0; i < binding.parameters.size(); ++i)
                {
                    fn.returnType = analysis::SubstituteTypeParam(fn.returnType, binding.parameters[i], binding.arguments[i]);
                    for (auto &param : fn.parameters)
                    {
                        param.typeName = analysis::SubstituteTypeParam(param.typeName, binding.parameters[i], binding.arguments[i]);
                        param.baseTypeName = analysis::SubstituteTypeParam(param.baseTypeName, binding.parameters[i], binding.arguments[i]);
                    }
                }
            }
            else if (!templateArgs.empty())
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

        struct AccessSegment
        {
            std::string name;
            bool isCall = false;
            size_t indexCount = 0;
        };

        /**
         * @brief Parses a chained member access expression into individual segments.
         * @param chain The string of the chain including trailing dot or arrow.
         * @return Vector of segments with names, call flags, and index counts.
         */
        std::vector<AccessSegment> ParseAccessChain(std::string_view chain)
        {
            std::vector<AccessSegment> segments;
            size_t i = 0;
            while (i < chain.size())
            {
                while (i < chain.size() && (chain[i] == ' ' || chain[i] == '\t'))
                {
                    ++i;
                }
                if (i >= chain.size())
                {
                    break;
                }

                size_t nameStart = i;
                if (!isalpha(static_cast<unsigned char>(chain[i])) && chain[i] != '_')
                {
                    break;
                }
                while (i < chain.size() && (isalnum(static_cast<unsigned char>(chain[i])) || chain[i] == '_'))
                {
                    ++i;
                }
                std::string name(chain.substr(nameStart, i - nameStart));
                AccessSegment seg;
                seg.name = std::move(name);

                while (i < chain.size() && (chain[i] == '(' || chain[i] == '['))
                {
                    char open = chain[i];
                    char close = (open == '(') ? ')' : ']';
                    if (open == '(')
                    {
                        seg.isCall = true;
                    }
                    else
                    {
                        ++seg.indexCount;
                    }
                    ++i;
                    int depth = 1;
                    while (i < chain.size() && depth > 0)
                    {
                        if (chain[i] == open)
                        {
                            ++depth;
                        }
                        else if (chain[i] == close)
                        {
                            --depth;
                        }
                        ++i;
                    }
                }

                while (i < chain.size() && (chain[i] == ' ' || chain[i] == '\t'))
                {
                    ++i;
                }

                if (i < chain.size() && chain[i] == '.')
                {
                    ++i;
                    segments.push_back(std::move(seg));
                }
                else if (i + 1 < chain.size() && chain[i] == '-' && chain[i + 1] == '>')
                {
                    i += 2;
                    segments.push_back(std::move(seg));
                }
                else
                {
                    break;
                }
            }
            return segments;
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
                          const std::string &snippet = "",
                          const std::string &sortText = "")
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
            if (!sortText.empty())
            {
                item.sortText = sortText;
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

        /**
         * @brief Builds `Name(${1:int a}, ${2:bool b})$0` for a function, or `Name()$0` with none.
         *
         * The placeholder text is the parameter as it was declared - its type and its name - so the
         * hint the user tabs through says what belongs there rather than `arg1`. The signature is
         * already in the symbol table; without this the item inserts a bare name and the call has
         * to be finished by hand.
         *
         * Returns an empty string when there is nothing useful to insert, which is the caller's
         * signal to leave the item as a plain name.
         */
        std::string CallSnippet(const std::string &name, const std::vector<analysis::ParameterInformation> &params)
        {
            if (name.empty())
            {
                return {};
            }

            std::string snippet = name + "(";
            for (size_t i = 0; i < params.size(); ++i)
            {
                if (i > 0)
                {
                    snippet += ", ";
                }

                std::string label = params[i].typeName;
                if (!params[i].name.empty())
                {
                    label += label.empty() ? params[i].name : " " + params[i].name;
                }
                if (label.empty())
                {
                    label = "arg" + std::to_string(i + 1);
                }

                // `}` and `$` end a placeholder, and a default value or a template argument can
                // contain either. Escaped, they stay text.
                std::string escaped;
                for (char c : label)
                {
                    if (c == '}' || c == '$' || c == '\\')
                    {
                        escaped += '\\';
                    }
                    escaped += c;
                }

                snippet += "${" + std::to_string(i + 1) + ":" + escaped + "}";
            }
            snippet += ")$0";
            return snippet;
        }

        /** @brief The primitive type names, for the contexts where only a type may be written. */
        const std::vector<std::string> &GetPrimitiveTypeNames()
        {
            // The primitives, plus the three types the standard add-ons register. Those three are
            // not primitives and are not in parser/Primitives.h for that reason; they are here
            // because in a position where only a type may be written, they are what people reach
            // for next.
            static const std::vector<std::string> primitives = []
            {
                std::vector<std::string> all;
                all.reserve(parser::primitives::k_all.size() + 3);
                for (const std::string_view name : parser::primitives::k_all)
                    all.emplace_back(name);
                all.emplace_back("string");
                all.emplace_back("array");
                all.emplace_back("dictionary");
                return all;
            }();
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
        /**
         * @brief The call snippet for a function known only by name, looked up in the symbol table.
         *
         * The scope tree answers completion inside a function body, and a definition there carries
         * a name and a type but no parameter list. The signature is one lookup away, and this is
         * the same arrangement TemplateSnippetForName uses immediately below for types.
         *
         * Empty when the name is not a function, or is overloaded: with more than one signature
         * there is no single call to insert, and guessing one would put the wrong arguments in the
         * user's file.
         */
        std::string CallSnippetForName(const std::string &name, const analysis::SymbolTable &table)
        {
            const auto symbols = table.FindSymbolsPtr(name);
            if (!symbols)
            {
                return {};
            }
            const analysis::Symbol *only = nullptr;
            for (const auto &sym : *symbols)
            {
                if (sym.type != analysis::SymbolType::Function)
                {
                    continue;
                }
                if (only != nullptr)
                {
                    return {};
                }
                only = &sym;
            }

            return only == nullptr ? std::string{} : CallSnippet(only->name, only->GetFunction().parameters);
        }

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
            // Was a 63-word copy that left out `and`, `or`, `not`, `xor` and `is` - the word
            // operators - so completion never offered them. Built from the language's own words
            // now, plus `string`, which is not a keyword but is the type people reach for most and
            // is worth offering next to the primitives.
            static const std::vector<std::string> keywords = []
            {
                std::vector<std::string> all;
                all.reserve(parser::keywords::k_reserved.size() +
                            parser::keywords::k_contextual.size() + 1);
                for (const std::string_view word : parser::keywords::k_reserved)
                    all.emplace_back(word);
                for (const std::string_view word : parser::keywords::k_contextual)
                    all.emplace_back(word);
                all.emplace_back("string");
                return all;
            }();
            return keywords;
        }
    }

    /**
     * @brief Completes the file path inside an `#include "..."`.
     *
     * Reported as missing: typing `#include ""` offered nothing, because the rule below it - no
     * completion inside a string literal - is right about every other string in the language and
     * this is the one place it is wrong.
     *
     * Every candidate is offered as a path relative to the file being edited, which is how an
     * include has to be written: a file one directory up comes back as `../shared.as`, one in a
     * subdirectory as `weapons/rifle.as`. The label carries that same relative path, so the list
     * reads the way the line will.
     *
     * @return The items, or nullopt when the cursor is not inside an include's quotes - which is
     *         the distinction the caller needs, since "inside an include with nothing to offer" and
     *         "not in an include" must not lead to the same fallback.
     */
    std::optional<std::vector<lsp::CompletionItem>> CompleteIncludePath(const CompletionRequest &request,
                                                                        const std::string &linePrefix)
    {
        if (request.documentPath.empty() || !request.listIncludeCandidates)
        {
            return std::nullopt;
        }

        // `#` and the name with nothing between them, because the compiler accepts nothing between
        // them either - `# include "helper.as"` is not a directive at all. Offering completion
        // there would say the line works while the analyzer calls it an error.
        static const std::regex includePrefixRegex(R"(^[ \t]*#include[ \t]*"([^"]*)$)");
        std::smatch match;
        if (!std::regex_search(linePrefix, match, includePrefixRegex))
        {
            return std::nullopt;
        }

        const std::string typed = match[1].str();

        std::vector<lsp::CompletionItem> items;

        std::error_code ec;
        const std::filesystem::path fromDirectory =
            std::filesystem::path(request.documentPath).parent_path();

        // Where the replacement starts: the character after the opening quote. Everything the user
        // has typed inside the quotes is replaced, so `#include "wea` completes to the whole
        // `weapons/rifle.as` rather than gluing a second copy of the prefix on.
        const auto quoteColumn = static_cast<lsp::uint>(linePrefix.size() - typed.size());

        std::unordered_set<std::string> offered;

        for (const std::string &candidate : request.listIncludeCandidates())
        {
            if (candidate.empty())
            {
                continue;
            }

            // The file being edited is not a candidate for its own include list.
            if (utils::IncludeResolver::NormalizePath(candidate) ==
                utils::IncludeResolver::NormalizePath(request.documentPath))
            {
                continue;
            }

            std::filesystem::path relative =
                std::filesystem::relative(std::filesystem::path(candidate), fromDirectory, ec);
            if (ec || relative.empty())
            {
                ec.clear();
                continue;
            }

            // Forward slashes on every platform. Windows accepts them, and a backslash inside a
            // string literal is an escape - `#include "sub\rifle.as"` carries a carriage return.
            std::string insertText = relative.generic_string();

            // A host that resolves the extension itself wants the short spelling, and the long one
            // would not open. See CompletionRequest::implicitExtension.
            if (!request.implicitExtension.empty() &&
                insertText.size() > request.implicitExtension.size() &&
                insertText.ends_with(request.implicitExtension))
            {
                insertText.resize(insertText.size() - request.implicitExtension.size());
            }

            if (!offered.insert(insertText).second)
            {
                continue;
            }

            lsp::CompletionItem item;
            item.label = insertText;
            item.kind = lsp::CompletionItemKind::File;
            item.detail = candidate;

            // A path in the same directory sorts above one reached through `..`, which is the order
            // a reader expects and not the order a plain string sort gives.
            item.sortText = (insertText.starts_with("../") ? "1" : "0") + insertText;

            lsp::TextEdit edit;
            edit.range.start.line = request.position.line;
            edit.range.start.character = quoteColumn;
            edit.range.end.line = request.position.line;
            edit.range.end.character = request.position.character;
            edit.newText = insertText;
            item.textEdit = edit;

            items.push_back(std::move(item));
        }

        return items;
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

        // asEP_PROPERTY_ACCESSOR_MODE: 0 and 1 leave script-defined accessors out of the
        // language entirely, 3 wants the `property` keyword written, and 2 - this server's
        // default - takes the name alone. Read from the config the analyzer reads, so the
        // completion list and the diagnostics cannot disagree about what a property is.
        const int accessorMode = request.config ? request.config->engine.propertyAccessorMode : 2;
        const bool accessorsAreProperties = accessorMode >= 2;
        const bool accessorKeywordRequired = accessorMode == 3;

        // 0. Positions where nothing may be completed. A comment, a string literal and the colon
        //    of a `case` label are all places the language has no symbol for, and each of them
        //    fell through to the global fallback below and answered with the entire scope.
        const size_t cursorOffset =
            utils::LineStartOffset(request.sourceCode, request.position.line) + prefix.size();
        //    Before that guard, not after: an include path lives inside a string literal, so the
        //    rule below is what made `#include ""` offer nothing at all. It is still the right
        //    rule for every other string in the language.
        if (auto includeItems = CompleteIncludePath(request, prefix))
        {
            return *includeItems;
        }

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

        // 2. Check for Member Access Context: "receiver." or "receiver->" or chained like "a.b.c." (with optional partial identifier)
        static const std::regex memberChainRegex(R"(((?:[a-zA-Z_][a-zA-Z0-9_]*(?:\([^\)]*\)|\[[^\]]*\])*\s*(?:\.|\->)\s*)+)([a-zA-Z_][a-zA-Z0-9_]*)?$)");
        std::smatch memberMatch;
        if (std::regex_search(prefix, memberMatch, memberChainRegex))
        {
            std::string chainFull = memberMatch[1].str();
            auto segments = ParseAccessChain(chainFull);

            std::string rawTypeName;

            if (!segments.empty())
            {
                std::string arrayContainer = (request.config && !request.config->types.arrayTypeName.empty()) ? request.config->types.arrayTypeName : "array";

                // Resolve base segment (segments[0])
                const auto &seg0 = segments[0];
                if (seg0.name == "this")
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
                    const analysis::LocalDefinition *def = analysis::ResolveInScope(innermostScope, seg0.name);
                    if (def && !def->typeName.empty())
                    {
                        rawTypeName = def->typeName;
                    }
                }

                if (rawTypeName.empty())
                {
                    auto globSyms = request.symbolTable.FindSymbols(seg0.name);
                    for (const auto &sym : globSyms)
                    {
                        if (sym.type == analysis::SymbolType::Variable && sym.containerName.empty())
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

                if (rawTypeName.empty() && accessorsAreProperties)
                {
                    auto globalAccessors = analysis::FindGlobalPropertyAccessors(seg0.name, request.symbolTable, accessorKeywordRequired);
                    if (!globalAccessors.empty())
                    {
                        rawTypeName = analysis::PropertyTypeFromAccessors(globalAccessors);
                    }
                }

                if (rawTypeName.empty() && seg0.isCall)
                {
                    auto fnSyms = request.symbolTable.FindSymbols(seg0.name);
                    for (const auto &sym : fnSyms)
                    {
                        if (sym.type == analysis::SymbolType::Function && sym.containerName.empty())
                        {
                            rawTypeName = sym.GetFunction().returnType;
                            break;
                        }
                    }
                }

                if (!rawTypeName.empty())
                {
                    rawTypeName = analysis::ResolveIndexedType(rawTypeName, seg0.indexCount, request.symbolTable, arrayContainer);
                }

                // Resolve subsequent chained segments
                for (size_t s = 1; s < segments.size() && !rawTypeName.empty(); ++s)
                {
                    const auto &seg = segments[s];
                    std::string cleanType = analysis::CleanBaseType(rawTypeName);
                    auto hierarchy = GetInheritedTypeHierarchy(request.symbolTable, cleanType);
                    std::string nextTypeName;

                    for (const auto &typeName : hierarchy)
                    {
                        auto memberSyms = request.symbolTable.FindSymbols(typeName + "::" + seg.name);
                        for (const auto &sym : memberSyms)
                        {
                            if (sym.type == analysis::SymbolType::Variable)
                            {
                                nextTypeName = sym.GetVariable().typeName;
                                break;
                            }
                            else if (sym.type == analysis::SymbolType::Function && seg.isCall)
                            {
                                nextTypeName = sym.GetFunction().returnType;
                                break;
                            }
                        }
                        if (!nextTypeName.empty())
                        {
                            break;
                        }

                        if (accessorsAreProperties)
                        {
                            auto accessors = analysis::FindPropertyAccessors(typeName, seg.name, request.symbolTable, accessorKeywordRequired);
                            if (!accessors.empty())
                            {
                                nextTypeName = analysis::PropertyTypeFromAccessors(accessors);
                                break;
                            }
                        }

                        for (const auto &sym : memberSyms)
                        {
                            if (sym.type == analysis::SymbolType::Function)
                            {
                                nextTypeName = sym.GetFunction().returnType;
                                break;
                            }
                        }
                        if (!nextTypeName.empty())
                        {
                            break;
                        }
                    }

                    if (nextTypeName.empty())
                    {
                        rawTypeName.clear();
                        break;
                    }

                    rawTypeName = analysis::ResolveIndexedType(nextTypeName, seg.indexCount, request.symbolTable, arrayContainer);
                }
            }

            if (!rawTypeName.empty())
            {
                std::string arrayContainer = (request.config && !request.config->types.arrayTypeName.empty()) ? request.config->types.arrayTypeName : "array";
                std::string canonicalType = CanonicalizeArrayType(rawTypeName, arrayContainer);

                auto targetTemplate = analysis::ParseTemplateType(canonicalType);
                std::string baseContainer = targetTemplate.containerName;
                std::vector<std::string> templateArgs = targetTemplate.templateArgs;
                const auto binding = analysis::BindTemplateArguments(canonicalType, request.symbolTable);

                auto substituteParams = [&](std::string text) -> std::string
                {
                    if (binding.usable)
                    {
                        for (size_t i = 0; i < binding.parameters.size(); ++i)
                        {
                            text = analysis::SubstituteTypeParam(text, binding.parameters[i], binding.arguments[i]);
                        }
                        return text;
                    }
                    if (!templateArgs.empty())
                    {
                        return analysis::SubstituteTypeParam(text, "T", templateArgs[0]);
                    }
                    return text;
                };

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
                                    detail = FormatMethodDetail(sym, binding, templateArgs);
                                }
                                else if (sym.type == analysis::SymbolType::Variable)
                                {
                                    kind = lsp::CompletionItemKind::Field;
                                    const auto &var = sym.GetVariable();
                                    detail = substituteParams(var.typeName);
                                }
                                else if (sym.type == analysis::SymbolType::Property)
                                {
                                    kind = lsp::CompletionItemKind::Property;
                                }

                                AddItemIfNew(items, seenLabels, sym.name, kind, detail, "", sym.qualifiedName);

                                // And `get_X`/`set_X` offer `X` as well. Nothing in the symbol table
                                // is called `X` - it holds the two methods - so a list built from
                                // the table alone offers every spelling except the one the user was
                                // reaching for and the compiler accepts.
                                if (!accessorsAreProperties)
                                {
                                    continue;
                                }

                                const std::string propertyName =
                                    analysis::PropertyNameFromAccessor(sym, accessorKeywordRequired);
                                if (propertyName.empty())
                                {
                                    continue;
                                }

                                std::string propertyType = analysis::PropertyTypeFromAccessors(
                                    analysis::FindPropertyAccessors(typeName, propertyName,
                                                                    request.symbolTable,
                                                                    accessorKeywordRequired));
                                propertyType = substituteParams(propertyType);

                                // Resolved against the accessor, so the doc comment written on the
                                // getter is the one the user reads on the property.
                                AddItemIfNew(items, seenLabels, propertyName,
                                             lsp::CompletionItemKind::Property, propertyType,
                                             "", sym.qualifiedName);
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
                        if (request.snippetSupport)
                        {
                            snippet = CallSnippetForName(def.name, request.symbolTable);
                        }
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
                        if (request.snippetSupport)
                        {
                            snippet = CallSnippet(sym.name, sym.GetFunction().parameters);
                        }
                        if (accessorsAreProperties)
                        {
                            const std::string propName = analysis::PropertyNameFromAccessor(sym, accessorKeywordRequired);
                            if (!propName.empty())
                            {
                                std::string propType = analysis::PropertyTypeFromAccessors(
                                    analysis::FindGlobalPropertyAccessors(propName, request.symbolTable, accessorKeywordRequired));
                                AddItemIfNew(items, seenLabels, propName,
                                             lsp::CompletionItemKind::Property, propType,
                                             "", sym.qualifiedName);
                            }
                        }
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

        // D. AngelScript keywords, and the snippets that write the declaration for you
        //
        // Both are offered under the SAME label - `class` the snippet and `class` the word - and
        // sortText decides the order: "0class" before "1class", so the one that writes a class with
        // its constructor and destructor comes first and the bare keyword stays one line below it.
        // A completion item is identified by its data and told apart in the list by its kind and
        // detail, not by its label, so two items may share one.
        //
        // That is why these four are pushed directly instead of through AddItemIfNew, and why they
        // deliberately do not claim their label in seenLabels: that helper dedupes on the label,
        // which is the right rule for symbols - a name is a name - and the wrong one here, because
        // claiming `class` would delete the bare keyword from the loop below.
        //
        // Every body below was measured against angelscript_oracle before it was written here; the
        // destructor in particular is not an assumption.
        if (request.snippetSupport)
        {
            const std::pair<const char *, const char *> declarationSnippets[] = {
                { "class",
                  "class ${1:Name}\n"
                  "{\n"
                  "    ${1:Name}()\n"
                  "    {\n"
                  "        $0\n"
                  "    }\n"
                  "\n"
                  "    ~${1:Name}()\n"
                  "    {\n"
                  "    }\n"
                  "}" },
                { "interface",
                  "interface ${1:Name}\n"
                  "{\n"
                  "    void ${2:DoThing}();\n"
                  "}" },
                { "mixin",
                  "mixin class ${1:Name}\n"
                  "{\n"
                  "    $0\n"
                  "}" },
                // No trailing `;`. An anonymous function is legal only where a funcdef is expected -
                // as an argument, or as the initialiser of a funcdef handle - and a semicolon here
                // would write the one shape the compiler rejects, which as-err-standalone-anonymous
                // -function now reports.
                { "function",
                  "function(${1:int value})\n"
                  "{\n"
                  "    $0\n"
                  "}" },
                { "enum",
                  "enum ${1:Name}\n"
                  "{\n"
                  "    ${2:Member} = 0\n"
                  "}" },
                { "funcdef",
                  "funcdef ${1:void} ${2:Name}(${3:int value});" },
                { "switch",
                  "switch (${1:value})\n"
                  "{\n"
                  "case ${2:0}:\n"
                  "    $0\n"
                  "    break;\n"
                  "\n"
                  "default:\n"
                  "    break;\n"
                  "}" },
                { "if",
                  "if (${1:condition})\n"
                  "{\n"
                  "    $0\n"
                  "}" },
                { "else",
                  "else\n"
                  "{\n"
                  "    $0\n"
                  "}" },
                { "for",
                  "for (uint ${1:i} = 0; ${1:i} < ${2:count}; ${1:i}++)\n"
                  "{\n"
                  "    $0\n"
                  "}" },
                { "while",
                  "while (${1:condition})\n"
                  "{\n"
                  "    $0\n"
                  "}" },
                { "do",
                  "do\n"
                  "{\n"
                  "    $0\n"
                  "}\n"
                  "while (${1:condition});" },
                { "try",
                  "try\n"
                  "{\n"
                  "    $0\n"
                  "}\n"
                  "catch\n"
                  "{\n"
                  "}" },
            };

            const std::pair<const char *, const char *> snippetDetails[] = {
                { "class", "declaration, with a constructor and a destructor" },
                { "interface", "declaration, with one method" },
                { "mixin", "mixin class declaration" },
                { "function", "anonymous function, for a funcdef parameter or handle" },
                { "enum", "declaration, with one member" },
                { "funcdef", "function-pointer type declaration" },
                { "switch", "block, with a case and a default" },
                { "if", "block" },
                { "else", "block" },
                { "for", "loop over a counter" },
                { "while", "loop" },
                { "do", "loop, with the test at the end" },
                { "try", "block, with its catch" },
            };

            for (size_t i = 0; i < std::size(declarationSnippets); ++i)
            {
                lsp::CompletionItem item;
                item.label = declarationSnippets[i].first;
                item.kind = lsp::CompletionItemKindEnum(lsp::CompletionItemKind::Snippet);
                item.insertText = declarationSnippets[i].second;
                item.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
                item.sortText = std::string("0") + declarationSnippets[i].first;
                item.detail = snippetDetails[i].second;
                items.push_back(std::move(item));
            }

            // `#include` is a directive, not a keyword, so there is no bare item beside it and no
            // ordering to arrange - it is simply missing from completion altogether today.
            //
            // The cursor lands between the quotes because that is where the next thing the user
            // wants already lives: this handler has an include branch that answers with the paths
            // that resolve, and it fires on the string literal of a `#include`. Leaving `$1` inside
            // the quotes is what joins the two halves.
            lsp::CompletionItem include;
            include.label = "#include";
            include.kind = lsp::CompletionItemKindEnum(lsp::CompletionItemKind::Snippet);
            include.insertText = "#include \"$1\"";
            include.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
            include.sortText = "0#include";
            include.detail = "directive, with the path left open";
            items.push_back(std::move(include));
        }

        for (const auto &kw : GetKeywords())
        {
            AddItemIfNew(items, seenLabels, kw, lsp::CompletionItemKind::Keyword,
                         "", "", "", "", "1" + kw);
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
