#include "features/completion/CompletionHandler.h"
#include "analysis/SemanticHelpers.h"
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

        std::string CleanBaseType(std::string typeName)
        {
            while (!typeName.empty() && (typeName.back() == '@' || typeName.back() == '&' || typeName.back() == ' '))
            {
                typeName.pop_back();
            }
            if (typeName.starts_with("const "))
            {
                typeName = typeName.substr(6);
            }
            while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == ']'))
            {
                if (typeName.back() == ']')
                {
                    size_t bracket = typeName.rfind('[');
                    if (bracket != std::string::npos)
                    {
                        typeName = typeName.substr(0, bracket);
                    }
                    else
                    {
                        typeName.pop_back();
                    }
                }
                else
                {
                    typeName.pop_back();
                }
            }
            if (typeName.starts_with("array<") && typeName.ends_with(">"))
            {
                std::string inner = typeName.substr(6, typeName.size() - 7);
                return CleanBaseType(inner);
            }
            return typeName;
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
            std::vector<std::string> queue;

            std::string rootType = CleanBaseType(initialTypeName);
            if (rootType.empty())
            {
                return hierarchy;
            }

            visited.insert(rootType);
            queue.push_back(rootType);

            size_t head = 0;
            while (head < queue.size())
            {
                std::string curType = queue[head++];
                hierarchy.push_back(curType);

                auto symbols = symbolTable.FindSymbols(curType);
                for (const auto &sym : symbols)
                {
                    if (sym.type == analysis::SymbolType::Class)
                    {
                        const auto &cls = sym.GetClass();
                        for (const auto &base : cls.bases)
                        {
                            std::string cleanBase = CleanBaseType(base);
                            if (!cleanBase.empty() && visited.insert(cleanBase).second)
                            {
                                queue.push_back(cleanBase);
                            }
                        }
                    }
                    else if (sym.type == analysis::SymbolType::Interface)
                    {
                        const auto &iface = sym.GetInterface();
                        for (const auto &base : iface.inheritedInterfaces)
                        {
                            std::string cleanBase = CleanBaseType(base);
                            if (!cleanBase.empty() && visited.insert(cleanBase).second)
                            {
                                queue.push_back(cleanBase);
                            }
                        }
                    }
                }
            }

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
                          const std::string &doc = "")
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

                        AddItemIfNew(items, seenLabels, sym.name, kind, detail);
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
        static const std::regex memberAccessRegex(R"(([a-zA-Z_][a-zA-Z0-9_]*)(?:\.|\->)([a-zA-Z_][a-zA-Z0-9_]*)?$)");
        std::smatch memberMatch;
        if (std::regex_search(prefix, memberMatch, memberAccessRegex))
        {
            std::string receiverName = memberMatch[1].str();
            std::string receiverTypeName;

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
                                receiverTypeName = sym.name;
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
                    receiverTypeName = CleanBaseType(def->typeName);
                }
            }

            if (receiverTypeName.empty())
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
                            receiverTypeName = CleanBaseType(var.typeName);
                            break;
                        }
                    }
                }
            }

            if (!receiverTypeName.empty())
            {
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
                                    const auto &fn = sym.GetFunction();
                                    detail = fn.returnType + " " + sym.name + "(...)";
                                }
                                else if (sym.type == analysis::SymbolType::Variable)
                                {
                                    kind = lsp::CompletionItemKind::Field;
                                    const auto &var = sym.GetVariable();
                                    detail = var.typeName;
                                }
                                else if (sym.type == analysis::SymbolType::Property)
                                {
                                    kind = lsp::CompletionItemKind::Property;
                                }

                                AddItemIfNew(items, seenLabels, sym.name, kind, detail);
                            }
                        }
                    });
                };

                auto hierarchy = GetInheritedTypeHierarchy(request.symbolTable, receiverTypeName);
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
                    if (def.kind == analysis::LocalDefinitionKind::Parameter)
                    {
                        kind = lsp::CompletionItemKind::Variable;
                    }
                    else if (def.kind == analysis::LocalDefinitionKind::Function ||
                             def.kind == analysis::LocalDefinitionKind::Method)
                    {
                        kind = lsp::CompletionItemKind::Function;
                    }
                    AddItemIfNew(items, seenLabels, def.name, kind, def.typeName);
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
                    AddItemIfNew(items, seenLabels, sym.name, kind, detail);
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
}
