#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticContext.h"

namespace angel_lsp::analysis
{
    namespace
    {
        std::string GetNodeText(TSNode node, std::string_view sourceCode)
        {
            if (ts_node_is_null(node) || sourceCode.empty())
            {
                return "";
            }
            uint32_t start = ts_node_start_byte(node);
            uint32_t end = ts_node_end_byte(node);
            if (start < sourceCode.size() && end <= sourceCode.size() && start < end)
            {
                return std::string(sourceCode.substr(start, end - start));
            }
            return "";
        }
    }

    bool IsReservedKeyword(const std::string &name)
    {
        static const ankerl::unordered_dense::set<std::string> kReserved = {
            "and", "auto", "bool", "break", "case", "cast", "catch",
            "class", "const", "continue", "default", "do", "double",
            "else", "enum", "false", "float", "for", "foreach", "funcdef",
            "if", "import", "in", "inout", "int", "int8", "int16", "int32", "int64",
            "interface", "is", "mixin", "namespace", "not", "null",
            "or", "out", "private", "protected", "return", "switch",
            "true", "try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64",
            "using", "void", "while", "xor",
        };
        return kReserved.contains(name);
    }

    bool IsPrimitiveTypeName(const std::string &name)
    {
        static const ankerl::unordered_dense::set<std::string> kPrimitives = {
            "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64",
            "float", "double", "bool", "void"
        };
        return kPrimitives.contains(name);
    }

    InitializerItemKind ClassifyInitializerItem(std::string_view item)
    {
        if (item.empty())
        {
            return InitializerItemKind::NumericOrExpression;
        }

        if (item.starts_with("\"") || item.starts_with("'"))
        {
            return InitializerItemKind::StringLiteral;
        }

        if (item == "true" || item == "false")
        {
            return InitializerItemKind::BooleanLiteral;
        }

        if (item == "null")
        {
            return InitializerItemKind::NullLiteral;
        }

        if (item.starts_with("{"))
        {
            return InitializerItemKind::NestedInitializer;
        }

        return InitializerItemKind::NumericOrExpression;
    }

    bool IsMixinClass(std::string_view baseTypeName, const SymbolTable &table)
    {
        if (baseTypeName.empty())
        {
            return false;
        }

        std::string searchName(baseTypeName);
        auto symsPtr = table.FindSymbolsPtr(searchName);
        if (!symsPtr)
        {
            return false;
        }

        for (const auto &sym : *symsPtr)
        {
            if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
            {
                return true;
            }
        }
        return false;
    }

    bool IsKnownType(const std::string &baseName, const DiagnosticContext &ctx)
    {
        if (baseName.empty()) return true;
        if (IsPrimitiveTypeName(baseName)) return true;
        if (baseName == ctx.request.GetStringTypeName()) return true;
        if (baseName == ctx.request.GetArrayTypeName()) return true;
        if (ctx.request.symbolTable.HasSymbolAnywhere(baseName)) return true;
        return false;
    }

    std::string CleanBaseType(std::string_view typeName)
    {
        while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
        {
            typeName.remove_prefix(1);
        }
        while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t'))
        {
            typeName.remove_suffix(1);
        }

        if (typeName.starts_with("const "))
        {
            typeName.remove_prefix(6);
        }

        while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
        {
            typeName.remove_prefix(1);
        }

        std::string result(typeName);

        bool modified = true;
        while (modified)
        {
            modified = false;
            while (!result.empty() && (result.back() == '@' || result.back() == '&' || result.back() == ' ' || result.back() == '\t'))
            {
                result.pop_back();
                modified = true;
            }
            if (result.ends_with("[]"))
            {
                result.resize(result.size() - 2);
                modified = true;
            }
            else if (!result.empty() && result.back() == ']')
            {
                size_t bracket = result.rfind('[');
                if (bracket != std::string::npos)
                {
                    result = result.substr(0, bracket);
                    modified = true;
                }
            }
        }

        if (result.starts_with("array<") && result.ends_with(">"))
        {
            std::string inner = result.substr(6, result.size() - 7);
            return CleanBaseType(inner);
        }

        return result;
    }

    std::vector<std::string> GetInheritedTypeHierarchy(const std::string &className, const SymbolTable &symbolTable)
    {
        std::vector<std::string> hierarchy;
        ankerl::unordered_dense::set<std::string> visited;
        std::vector<std::string> queue;

        std::string rootType = CleanBaseType(className);
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
                if (sym.type == SymbolType::Class)
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
                else if (sym.type == SymbolType::Interface)
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

    std::vector<std::string> GetAllRelatedClasses(const std::string &className, const SymbolTable &symbolTable)
    {
        std::vector<std::string> related;
        ankerl::unordered_dense::set<std::string> visited;
        std::vector<std::string> queue;

        std::string rootType = CleanBaseType(className);
        if (rootType.empty())
        {
            return related;
        }

        visited.insert(rootType);
        queue.push_back(rootType);

        // 1. Traverse base classes and interfaces
        size_t head = 0;
        while (head < queue.size())
        {
            std::string curType = queue[head++];
            related.push_back(curType);

            auto symbols = symbolTable.FindSymbols(curType);
            for (const auto &sym : symbols)
            {
                if (sym.type == SymbolType::Class)
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
                else if (sym.type == SymbolType::Interface)
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

        // 2. Discover derived classes in SymbolTable that inherit from any known related class
        bool expanded = true;
        while (expanded)
        {
            expanded = false;
            symbolTable.ForEachSymbol(
                [&](const std::string &, const std::vector<Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.type == SymbolType::Class)
                        {
                            const auto &cls = sym.GetClass();
                            for (const auto &base : cls.bases)
                            {
                                std::string cleanBase = CleanBaseType(base);
                                std::string symIdentifier = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                                if (visited.contains(cleanBase) || visited.contains(base))
                                {
                                    if (visited.insert(symIdentifier).second)
                                    {
                                        related.push_back(symIdentifier);
                                        if (sym.name != symIdentifier)
                                        {
                                            visited.insert(sym.name);
                                            related.push_back(sym.name);
                                        }
                                        expanded = true;
                                        break;
                                    }
                                }
                            }
                        }
                        else if (sym.type == SymbolType::Interface)
                        {
                            const auto &iface = sym.GetInterface();
                            for (const auto &base : iface.inheritedInterfaces)
                            {
                                std::string cleanBase = CleanBaseType(base);
                                std::string symIdentifier = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                                if (visited.contains(cleanBase) || visited.contains(base))
                                {
                                    if (visited.insert(symIdentifier).second)
                                    {
                                        related.push_back(symIdentifier);
                                        if (sym.name != symIdentifier)
                                        {
                                            visited.insert(sym.name);
                                            related.push_back(sym.name);
                                        }
                                        expanded = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                });
        }

        return related;
    }

    std::vector<ContainerInfo> GetEnclosingContainers(TSNode node, std::string_view sourceCode)
    {
        std::vector<ContainerInfo> rawContainers;
        if (ts_node_is_null(node))
        {
            return rawContainers;
        }

        TSNode current = node;

        // If the node itself is the name of a class/interface/namespace declaration (e.g. 'class Player'),
        // we want its enclosing container to be the outer scope, not itself.
        TSNode parent = ts_node_parent(current);
        if (!ts_node_is_null(parent))
        {
            std::string_view parentType = ts_node_type(parent);
            if (parentType == "class_declaration" || parentType == "interface_declaration" || parentType == "namespace_declaration")
            {
                TSNode nameChild = ts_node_child_by_field_name(parent, "name", 4);
                if (!ts_node_is_null(nameChild) &&
                    ts_node_start_byte(nameChild) == ts_node_start_byte(current) &&
                    ts_node_end_byte(nameChild) == ts_node_end_byte(current))
                {
                    current = parent;
                }
            }
        }

        current = ts_node_parent(current);

        while (!ts_node_is_null(current))
        {
            std::string_view type = ts_node_type(current);
            ContainerKind kind = ContainerKind::Class;
            bool isContainer = false;

            if (type == "class_declaration")
            {
                kind = ContainerKind::Class;
                isContainer = true;
            }
            else if (type == "interface_declaration")
            {
                kind = ContainerKind::Interface;
                isContainer = true;
            }
            else if (type == "namespace_declaration")
            {
                kind = ContainerKind::Namespace;
                isContainer = true;
            }

            if (isContainer)
            {
                TSNode nameNode = ts_node_child_by_field_name(current, "name", 4);
                if (!ts_node_is_null(nameNode))
                {
                    uint32_t start = ts_node_start_byte(nameNode);
                    uint32_t end = ts_node_end_byte(nameNode);
                    if (start < sourceCode.size() && end <= sourceCode.size() && start < end)
                    {
                        std::string name(sourceCode.substr(start, end - start));
                        ContainerInfo info;
                        info.name = name;
                        info.kind = kind;
                        rawContainers.push_back(std::move(info));
                    }
                }
            }

            current = ts_node_parent(current);
        }

        // Compute qualified names from outermost to innermost
        for (size_t i = 0; i < rawContainers.size(); ++i)
        {
            std::string qName;
            for (size_t j = rawContainers.size(); j > i; --j)
            {
                if (!qName.empty())
                {
                    qName += "::";
                }
                qName += rawContainers[j - 1].name;
            }
            rawContainers[i].qualifiedName = qName;
        }

        return rawContainers;
    }

    std::vector<Symbol> FindSymbolsInScope(
        const SymbolTable &symbolTable,
        const std::vector<ContainerInfo> &containers,
        const std::string &name)
    {
        if (name.empty())
        {
            return {};
        }

        // 1. Container hierarchy lookup (from innermost to outermost)
        for (const auto &container : containers)
        {
            if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
            {
                auto hierarchy = GetInheritedTypeHierarchy(container.qualifiedName, symbolTable);
                for (const auto &cls : hierarchy)
                {
                    std::string qName = cls + "::" + name;
                    auto found = symbolTable.FindSymbols(qName);
                    if (!found.empty())
                    {
                        return found;
                    }
                }

                if (container.name != container.qualifiedName)
                {
                    auto bareHierarchy = GetInheritedTypeHierarchy(container.name, symbolTable);
                    for (const auto &cls : bareHierarchy)
                    {
                        std::string qName = cls + "::" + name;
                        auto found = symbolTable.FindSymbols(qName);
                        if (!found.empty())
                        {
                            return found;
                        }
                    }
                }
            }
            else if (container.kind == ContainerKind::Namespace)
            {
                std::string qName = container.qualifiedName + "::" + name;
                auto found = symbolTable.FindSymbols(qName);
                if (!found.empty())
                {
                    return found;
                }
            }
        }

        // 2. Global lookup
        auto globalFound = symbolTable.FindSymbols(name);
        if (!globalFound.empty())
        {
            return globalFound;
        }

        // 3. Enum member fallback
        std::vector<Symbol> enumMembers;
        symbolTable.ForEachSymbol(
            [&](const std::string &, const std::vector<Symbol> &symList)
            {
                for (const auto &sym : symList)
                {
                    if (sym.type == SymbolType::Enum)
                    {
                        const auto &en = sym.GetEnum();
                        for (const auto &m : en.members)
                        {
                            if (m.name == name)
                            {
                                enumMembers.push_back(sym);
                                break;
                            }
                        }
                    }
                }
            });
        if (!enumMembers.empty())
        {
            return enumMembers;
        }

        return {};
    }

    std::vector<Symbol> FindSymbolsInScope(
        const std::string &name,
        TSNode node,
        std::string_view sourceCode,
        const SymbolTable &symbolTable)
    {
        std::vector<ContainerInfo> containers = GetEnclosingContainers(node, sourceCode);
        return FindSymbolsInScope(symbolTable, containers, name);
    }

    std::string ResolveExpressionType(
        TSNode exprNode,
        const Scope *scope,
        const SymbolTable &symbolTable,
        std::string_view sourceCode,
        std::string_view uri)
    {
        if (ts_node_is_null(exprNode) || sourceCode.empty())
        {
            return "";
        }

        std::string_view nodeType = ts_node_type(exprNode);

        if (nodeType == "identifier")
        {
            std::string name = GetNodeText(exprNode, sourceCode);
            if (name == "this")
            {
                auto containers = GetEnclosingContainers(exprNode, sourceCode);
                for (const auto &c : containers)
                {
                    if (c.kind == ContainerKind::Class)
                    {
                        return c.name;
                    }
                }
                return "";
            }

            if (scope)
            {
                const LocalDefinition *def = ResolveInScope(scope, name);
                if (def && !def->typeName.empty())
                {
                    return CleanBaseType(def->typeName);
                }
            }

            auto syms = symbolTable.FindSymbols(name);
            for (const auto &sym : syms)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) && !sym.GetVariable().typeName.empty())
                {
                    return CleanBaseType(sym.GetVariable().typeName);
                }
                else if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                {
                    return CleanBaseType(sym.GetFunction().returnType);
                }
            }
            return "";
        }

        if (nodeType == "member_expression")
        {
            // member_expression names both children unconditionally in the grammar, so the fields
            // are the answer. Falling back to positional children was strictly worse: named_child(1)
            // is not the member once anything else lands between the two, and a null field here
            // means a malformed tree worth surfacing rather than guessing around.
            TSNode objNode = ts_node_child_by_field_name(exprNode, "object", 6);
            TSNode memNode = ts_node_child_by_field_name(exprNode, "member", 6);
            if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
            {
                return "";
            }

            std::string objType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri);
            objType = CleanBaseType(objType);
            if (objType.empty())
            {
                return "";
            }

            std::string memName = GetNodeText(memNode, sourceCode);
            while (!memName.empty() && isspace(static_cast<unsigned char>(memName.front()))) memName.erase(memName.begin());
            while (!memName.empty() && isspace(static_cast<unsigned char>(memName.back()))) memName.pop_back();
            auto hierarchy = GetInheritedTypeHierarchy(objType, symbolTable);
            for (const auto &typeName : hierarchy)
            {
                std::string qName = typeName + "::" + memName;
                auto found = symbolTable.FindSymbols(qName);
                if (found.empty())
                {
                    auto allSymbols = symbolTable.FindSymbols(memName);
                    for (const auto &sSym : allSymbols)
                    {
                        if (sSym.containerName == typeName)
                        {
                            found.push_back(sSym);
                        }
                    }
                }
                for (const auto &sym : found)
                {
                    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) && !sym.GetVariable().typeName.empty())
                    {
                        return CleanBaseType(sym.GetVariable().typeName);
                    }
                    else if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                    {
                        return CleanBaseType(sym.GetFunction().returnType);
                    }
                }
            }
            return "";
        }

        if (nodeType == "call_expression")
        {
            TSNode funcNode = ts_node_child_by_field_name(exprNode, "function", 8);
            if (ts_node_is_null(funcNode) && ts_node_child_count(exprNode) > 0)
            {
                funcNode = ts_node_child(exprNode, 0);
            }
            if (!ts_node_is_null(funcNode))
            {
                return ResolveExpressionType(funcNode, scope, symbolTable, sourceCode, uri);
            }
            return "";
        }

        if (nodeType == "parenthesized_expression")
        {
            uint32_t count = ts_node_child_count(exprNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(exprNode, i);
                std::string_view cType = ts_node_type(child);
                if (cType != "(" && cType != ")")
                {
                    return ResolveExpressionType(child, scope, symbolTable, sourceCode, uri);
                }
            }
            return "";
        }

        return "";
    }
}


