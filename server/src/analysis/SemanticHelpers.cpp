#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticContext.h"
#include "utils/Utils.h"

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

    bool IsFromPredefinedStub(const Symbol &sym, const DiagnosticContext &ctx)
    {
        return utils::IsPredefinedFile(sym.fileUri, ctx.request.predefinedFileExtension);
    }

    bool IsDestructorDeclaration(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const std::string_view source = ctx.request.sourceCode;
        if (source.empty() || sym.fileUri != ctx.request.fileUri)
        {
            return false;
        }

        // Walk to the start of the declaration's line, then to the identifier.
        size_t offset = 0;
        for (uint32_t line = 0; line < sym.selectionRange.startLine; ++line)
        {
            offset = source.find('\n', offset);
            if (offset == std::string_view::npos)
            {
                return false;
            }
            ++offset;
        }

        offset += sym.selectionRange.startCharacter;
        if (offset == 0 || offset > source.size())
        {
            return false;
        }

        size_t back = offset;
        while (back > 0 && (source[back - 1] == ' ' || source[back - 1] == '\t'))
        {
            --back;
        }
        return back > 0 && source[back - 1] == '~';
    }

    std::string_view FirstAttributeName(const SymbolModifiers &modifiers)
    {
        // Grammar order: choice("override", "final", "explicit", "property", "delete").
        if (modifiers.isOverride) { return "override"; }
        if (modifiers.isFinal)    { return "final"; }
        if (modifiers.isExplicit) { return "explicit"; }
        if (modifiers.isProperty) { return "property"; }
        if (modifiers.isDelete)   { return "delete"; }
        return {};
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

        // `this` is its own production, not an identifier spelled "this".
        if (nodeType == "this_expression")
        {
            for (const auto &container : GetEnclosingContainers(exprNode, sourceCode))
            {
                if (container.kind == ContainerKind::Class)
                {
                    return container.name;
                }
            }
            return "";
        }

        // Every name written in an expression arrives wrapped in scoped_identifier, qualified or
        // not - the grammar admits no bare identifier in expression position. Without this branch
        // the whole identifier case below was unreachable from real source, and everything built on
        // it resolved nothing: the type of `myClass` in `myClass.f`, of a variable used as an
        // initializer, of an argument in a constructor call.
        if (nodeType == "scoped_identifier")
        {
            // A qualification is asked for whole first, since `Game::config` is the key the
            // collector stored it under. Only if that finds nothing does the last segment answer,
            // which is both the unqualified case and the best reading of a namespace this analyzer
            // cannot see.
            TSNode lastIdentifier = {};
            bool qualified = false;
            const uint32_t childCount = ts_node_named_child_count(exprNode);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_named_child(exprNode, i);
                if (std::string_view(ts_node_type(child)) == "identifier")
                {
                    qualified = !ts_node_is_null(lastIdentifier);
                    lastIdentifier = child;
                }
            }

            if (qualified)
            {
                const std::string whole = GetNodeText(exprNode, sourceCode);
                for (const auto &sym : symbolTable.FindSymbols(whole))
                {
                    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                        !sym.GetVariable().typeName.empty())
                    {
                        return CleanBaseType(sym.GetVariable().typeName);
                    }
                    if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                    {
                        return CleanBaseType(sym.GetFunction().returnType);
                    }
                }
            }

            return ts_node_is_null(lastIdentifier)
                       ? std::string()
                       : ResolveExpressionType(lastIdentifier, scope, symbolTable, sourceCode, uri);
        }

        if (nodeType == "identifier")
        {
            std::string name = GetNodeText(exprNode, sourceCode);
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

        // A cast names its own result: `cast<CBasePlayer@>(ent)` is a CBasePlayer whatever `ent`
        // was, which is the whole reason the construct exists and is how most of the corpus reaches
        // a derived type at all. The same holds for a constructor call and a primitive functional
        // cast - all three write the answer down, so none of them needs the operand resolved.
        if (nodeType == "cast_expression" || nodeType == "functional_cast_expression")
        {
            TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", 4);
            return ts_node_is_null(typeNode) ? std::string()
                                             : CleanBaseType(GetNodeText(typeNode, sourceCode));
        }

        if (nodeType == "construct_call_expression")
        {
            TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", 4);
            if (ts_node_is_null(typeNode))
            {
                return "";
            }

            // The template argument list is a sibling rather than part of the type field, and
            // CleanBaseType is what unwraps `array<Foo@>` down to Foo - so it has to see the whole
            // spelling to do it.
            std::string written = GetNodeText(typeNode, sourceCode);
            const uint32_t childCount = ts_node_named_child_count(exprNode);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_named_child(exprNode, i);
                if (std::string_view(ts_node_type(child)) == "template_type_list")
                {
                    written += GetNodeText(child, sourceCode);
                    break;
                }
            }
            return CleanBaseType(written);
        }

        // `arr[i]` is the element, and CleanBaseType already unwraps `array<T>` to T - so the
        // object's own answer is the element's answer, and indexing needs nothing further. A
        // container that is not an array resolves to the container, which finds no member and
        // leaves the caller silent rather than wrong.
        if (nodeType == "index_expression")
        {
            TSNode objNode = ts_node_child_by_field_name(exprNode, "object", 6);
            return ts_node_is_null(objNode)
                       ? std::string()
                       : ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri);
        }

        // `@handle`, `-x` and `++i` all carry their operand's type through. `!x` and `not x` are
        // the exception: they are bool whatever they were applied to.
        if (nodeType == "unary_expression")
        {
            TSNode operatorNode = ts_node_child_by_field_name(exprNode, "operator", 8);
            if (!ts_node_is_null(operatorNode))
            {
                const std::string op = GetNodeText(operatorNode, sourceCode);
                if (op == "!" || op == "not")
                {
                    return "bool";
                }
            }

            TSNode operandNode = ts_node_child_by_field_name(exprNode, "operand", 7);
            return ts_node_is_null(operandNode)
                       ? std::string()
                       : ResolveExpressionType(operandNode, scope, symbolTable, sourceCode, uri);
        }

        if (nodeType == "postfix_expression")
        {
            TSNode operandNode = ts_node_child_by_field_name(exprNode, "operand", 7);
            return ts_node_is_null(operandNode)
                       ? std::string()
                       : ResolveExpressionType(operandNode, scope, symbolTable, sourceCode, uri);
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

        // Everything else - a literal, a binary or ternary expression, a lambda, an initializer
        // list - yields nothing on purpose. A literal's members are engine-registered and so never
        // judged by the passes that ask this question; the rest need the engine's promotion and
        // operator-overload rules, and answering them by guess would turn every consumer's
        // deliberate silence into a wrong sentence.
        return "";
    }
}


