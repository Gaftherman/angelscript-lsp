#include "features/completion/CompletionHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/DocComment.h"
#include "analysis/SignatureFormatter.h"
#include <unordered_set>
#include <sstream>
#include <regex>

namespace angel_lsp::features
{
    namespace
    {
        bool IsInsideScope(const analysis::Scope &scope, uint32_t line, uint32_t character)
        {
            if (line < scope.startLine || line > scope.endLine)
            {
                return false;
            }
            if (line == scope.startLine && character < scope.startCharacter)
            {
                return false;
            }
            if (line == scope.endLine && character > scope.endCharacter)
            {
                return false;
            }
            return true;
        }

        const analysis::Scope *FindInnermostScope(const analysis::Scope *root, uint32_t line, uint32_t character)
        {
            if (!root || !IsInsideScope(*root, line, character))
            {
                return nullptr;
            }

            const analysis::Scope *current = root;
            bool foundChild = true;
            while (foundChild)
            {
                foundChild = false;
                for (const auto &child : current->children)
                {
                    if (child && IsInsideScope(*child, line, character))
                    {
                        current = child.get();
                        foundChild = true;
                        break;
                    }
                }
            }
            return current;
        }

        std::string CanonicalizeArrayType(std::string_view typeStr, const std::string &arrayTypeName = "array")
        {
            std::string s(typeStr);
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '@' || s.back() == '&')) s.pop_back();

            if (s.starts_with("const "))
            {
                s = s.substr(6);
            }

            while (s.ends_with("[]"))
            {
                std::string inner = s.substr(0, s.size() - 2);
                inner = CanonicalizeArrayType(inner, arrayTypeName);
                s = arrayTypeName + "<" + inner + ">";
            }
            return s;
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
                          const std::string &resolveKey = "")
        {
            if (label.empty() || seenLabels.contains(label))
            {
                return;
            }
            seenLabels.insert(label);

            lsp::CompletionItem item;
            item.label = label;
            item.kind = lsp::CompletionItemKindEnum(kind);
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

                auto addMembersForType = [&](const std::string &typeName)
                {
                    request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
                    {
                        for (const auto &sym : symList)
                        {
                            if (sym.containerName == typeName)
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
                    });
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

                    // A resolve key only for the function-like definitions, which are the ones the
                    // symbol table also holds and can therefore be resolved to a doc comment. A
                    // local variable has no symbol-table entry to look up, and this path runs first
                    // - so without the key here a module-level function would reach the client with
                    // no identity at all and never resolve.
                    AddItemIfNew(items, seenLabels, def.name, kind, def.typeName, "",
                                 isCallable ? def.name : std::string{});
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
                    switch (sym.type)
                    {
                    case analysis::SymbolType::Function:
                        kind = lsp::CompletionItemKind::Function;
                        detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
                        break;
                    case analysis::SymbolType::Class:
                        kind = lsp::CompletionItemKind::Class;
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
                    AddItemIfNew(items, seenLabels, sym.name, kind, detail, "", sym.qualifiedName);
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
