#include "analysis/SemanticHelpers.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticContext.h"
#include "utils/Utils.h"

namespace angel_lsp::analysis
{
    namespace
    {
        std::string CleanExpressionType(std::string_view typeName)
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
            while (!typeName.empty() && (typeName.back() == '@' || typeName.back() == '&' || typeName.back() == ' ' || typeName.back() == '\t'))
            {
                typeName.remove_suffix(1);
            }
            return std::string(typeName);
        }

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

    NonInstantiableKind ClassifyNonInstantiable(std::string_view baseTypeName, const SymbolTable &table)
    {
        if (baseTypeName.empty())
        {
            return NonInstantiableKind::None;
        }

        auto symsPtr = table.FindSymbolsPtr(std::string(baseTypeName));
        if (!symsPtr)
        {
            return NonInstantiableKind::None;
        }

        for (const auto &sym : *symsPtr)
        {
            if (sym.type == SymbolType::Interface)
            {
                return NonInstantiableKind::Interface;
            }
            if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isAbstract)
            {
                return NonInstantiableKind::Abstract;
            }
        }
        return NonInstantiableKind::None;
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

    TemplateTypeInfo ParseTemplateType(std::string_view typeName)
    {
        TemplateTypeInfo info;
        while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
        {
            typeName.remove_prefix(1);
        }
        while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t' || typeName.back() == '@' || typeName.back() == '&'))
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

        size_t openBracket = typeName.find('<');
        if (openBracket == std::string_view::npos || !typeName.ends_with('>'))
        {
            info.containerName = std::string(typeName);
            return info;
        }

        info.containerName = std::string(typeName.substr(0, openBracket));
        std::string_view inner = typeName.substr(openBracket + 1, typeName.size() - openBracket - 2);

        int depth = 0;
        size_t start = 0;
        for (size_t i = 0; i < inner.size(); ++i)
        {
            if (inner[i] == '<')
            {
                ++depth;
            }
            else if (inner[i] == '>')
            {
                --depth;
            }
            else if (inner[i] == ',' && depth == 0)
            {
                std::string_view arg = inner.substr(start, i - start);
                while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) arg.remove_prefix(1);
                while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) arg.remove_suffix(1);
                if (!arg.empty())
                {
                    info.templateArgs.push_back(std::string(arg));
                }
                start = i + 1;
            }
        }

        if (start < inner.size())
        {
            std::string_view arg = inner.substr(start);
            while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) arg.remove_prefix(1);
            while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) arg.remove_suffix(1);
            if (!arg.empty())
            {
                info.templateArgs.push_back(std::string(arg));
            }
        }

        return info;
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
            if (parentType == "class_declaration" || parentType == "interface_declaration" ||
                parentType == "namespace_declaration" || parentType == "mixin_declaration")
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

            // A mixin body is a class body for every question these helpers answer - what type
            // encloses this node, what `this` is, which methods an unqualified name can reach.
            // Leaving it out meant a mixin's whole body was treated as though it sat at file scope,
            // which the call-argument audit found: a method calling its own sibling matched an
            // unrelated global of the same name instead.
            if (type == "class_declaration" || type == "mixin_declaration")
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

        // `this` expression
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

        // String literal
        if (nodeType == "string_literal")
        {
            return "string";
        }

        // Boolean literal
        if (nodeType == "boolean_literal")
        {
            return "bool";
        }

        // Null literal
        if (nodeType == "null_literal")
        {
            return "null";
        }

        // Character literal
        if (nodeType == "character_literal")
        {
            return "uint8";
        }

        // Number literal
        if (nodeType == "number_literal")
        {
            std::string text = GetNodeText(exprNode, sourceCode);
            if (text.find('f') != std::string::npos || text.find('F') != std::string::npos)
            {
                return "float";
            }
            if (text.find('.') != std::string::npos || text.find('e') != std::string::npos || text.find('E') != std::string::npos)
            {
                return "double";
            }
            if ((text.find('u') != std::string::npos || text.find('U') != std::string::npos) &&
                (text.find('l') != std::string::npos || text.find('L') != std::string::npos))
            {
                return "uint64";
            }
            if (text.find('l') != std::string::npos || text.find('L') != std::string::npos || text.ends_with("i64"))
            {
                return "int64";
            }
            if (text.find('u') != std::string::npos || text.find('U') != std::string::npos)
            {
                return "uint";
            }
            return "int";
        }

        // Scoped identifier (e.g. Game::Player or bare identifier wrapped in scoped_identifier)
        if (nodeType == "scoped_identifier")
        {
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
                        return CleanExpressionType(sym.GetVariable().typeName);
                    }
                    if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                    {
                        return CleanExpressionType(sym.GetFunction().returnType);
                    }
                }
            }

            return ts_node_is_null(lastIdentifier)
                       ? std::string()
                       : ResolveExpressionType(lastIdentifier, scope, symbolTable, sourceCode, uri);
        }

        // Bare identifier
        if (nodeType == "identifier")
        {
            std::string name = GetNodeText(exprNode, sourceCode);
            if (scope)
            {
                const LocalDefinition *def = ResolveInScope(scope, name);
                if (def && !def->typeName.empty())
                {
                    return CleanExpressionType(def->typeName);
                }
            }

            auto syms = symbolTable.FindSymbols(name);
            for (const auto &sym : syms)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) && !sym.GetVariable().typeName.empty())
                {
                    return CleanExpressionType(sym.GetVariable().typeName);
                }
                else if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                {
                    return CleanExpressionType(sym.GetFunction().returnType);
                }
            }
            return "";
        }

        // Assignment expression (e.g. g_var = 0, x += 1)
        if (nodeType == "assignment_expression")
        {
            TSNode left = ts_node_child_by_field_name(exprNode, "left", 4);
            if (!ts_node_is_null(left))
            {
                return ResolveExpressionType(left, scope, symbolTable, sourceCode, uri);
            }
            return "";
        }

        // Binary expression (e.g. a + b, x == y, etc.)
        if (nodeType == "binary_expression")
        {
            TSNode left = ts_node_child_by_field_name(exprNode, "left", 4);
            TSNode opNode = ts_node_child_by_field_name(exprNode, "operator", 8);
            TSNode right = ts_node_child_by_field_name(exprNode, "right", 5);
            if (ts_node_is_null(left) || ts_node_is_null(right))
            {
                return "";
            }

            std::string op = GetNodeText(opNode, sourceCode);
            std::string leftType = ResolveExpressionType(left, scope, symbolTable, sourceCode, uri);
            std::string rightType = ResolveExpressionType(right, scope, symbolTable, sourceCode, uri);

            // Relational and equality operators always evaluate to bool
            if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" ||
                op == "is" || op == "!is")
            {
                return "bool";
            }

            // Logical operators always evaluate to bool
            if (op == "&&" || op == "||" || op == "and" || op == "or" || op == "^^" || op == "xor")
            {
                return "bool";
            }

            // String concatenation
            if (op == "+" && (CleanBaseType(leftType) == "string" || CleanBaseType(rightType) == "string"))
            {
                return "string";
            }

            std::string cleanLeft = CleanBaseType(leftType);
            std::string cleanRight = CleanBaseType(rightType);

            // Map binary operator to operator overload method name
            std::string opMethod;
            std::string revOpMethod;
            if (op == "+") { opMethod = "opAdd"; revOpMethod = "opAdd_r"; }
            else if (op == "-") { opMethod = "opSub"; revOpMethod = "opSub_r"; }
            else if (op == "*") { opMethod = "opMul"; revOpMethod = "opMul_r"; }
            else if (op == "/") { opMethod = "opDiv"; revOpMethod = "opDiv_r"; }
            else if (op == "%") { opMethod = "opMod"; revOpMethod = "opMod_r"; }
            else if (op == "**") { opMethod = "opPow"; revOpMethod = "opPow_r"; }
            else if (op == "&") { opMethod = "opAnd"; revOpMethod = "opAnd_r"; }
            else if (op == "|") { opMethod = "opOr"; revOpMethod = "opOr_r"; }
            else if (op == "^") { opMethod = "opXor"; revOpMethod = "opXor_r"; }
            else if (op == "<<") { opMethod = "opShl"; revOpMethod = "opShl_r"; }
            else if (op == ">>") { opMethod = "opShr"; revOpMethod = "opShr_r"; }
            else if (op == ">>>") { opMethod = "opUShr"; revOpMethod = "opUShr_r"; }

            // 1. Check member operator overloads on left operand
            if (!opMethod.empty() && !cleanLeft.empty())
            {
                std::vector<Symbol> candidates;
                auto hierarchy = GetInheritedTypeHierarchy(cleanLeft, symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    auto found = symbolTable.FindSymbols(typeName + "::" + opMethod);
                    for (const auto &sym : found)
                    {
                        if (sym.type == SymbolType::Function)
                        {
                            candidates.push_back(sym);
                        }
                    }
                }
                if (!candidates.empty())
                {
                    auto match = ResolveBestOverload(candidates, { rightType }, symbolTable);
                    if (match.bestCandidate && std::holds_alternative<FunctionSignature>(match.bestCandidate->signature))
                    {
                        return CleanExpressionType(match.bestCandidate->GetFunction().returnType);
                    }
                }
            }

            // 2. Check reversed operator overloads on right operand
            if (!revOpMethod.empty() && !cleanRight.empty())
            {
                std::vector<Symbol> candidates;
                auto hierarchy = GetInheritedTypeHierarchy(cleanRight, symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    auto found = symbolTable.FindSymbols(typeName + "::" + revOpMethod);
                    for (const auto &sym : found)
                    {
                        if (sym.type == SymbolType::Function)
                        {
                            candidates.push_back(sym);
                        }
                    }
                }
                if (!candidates.empty())
                {
                    auto match = ResolveBestOverload(candidates, { leftType }, symbolTable);
                    if (match.bestCandidate && std::holds_alternative<FunctionSignature>(match.bestCandidate->signature))
                    {
                        return CleanExpressionType(match.bestCandidate->GetFunction().returnType);
                    }
                }
            }

            // 3. Numeric promotions for primitives
            if (!cleanLeft.empty() && !cleanRight.empty())
            {
                if (cleanLeft == "double" || cleanRight == "double")
                {
                    return "double";
                }
                if (cleanLeft == "float" || cleanRight == "float")
                {
                    return "float";
                }
                if (cleanLeft == "uint64" || cleanRight == "uint64")
                {
                    return "uint64";
                }
                if (cleanLeft == "int64" || cleanRight == "int64")
                {
                    return "int64";
                }
                if (op == "<<" || op == ">>" || op == ">>>")
                {
                    return cleanLeft == "uint" || cleanLeft == "uint32" || cleanLeft == "uint64" || cleanLeft == "uint16" || cleanLeft == "uint8" ? "uint" : "int";
                }
                if (cleanLeft == "uint" || cleanRight == "uint" || cleanLeft == "uint32" || cleanRight == "uint32")
                {
                    return "uint";
                }
                if (IsPrimitiveTypeName(cleanLeft) && IsPrimitiveTypeName(cleanRight))
                {
                    return "int";
                }
            }

            return "";
        }

        // Ternary expression (e.g. cond ? expr1 : expr2)
        if (nodeType == "ternary_expression")
        {
            TSNode consequence = ts_node_child_by_field_name(exprNode, "consequence", 11);
            TSNode alternative = ts_node_child_by_field_name(exprNode, "alternative", 11);
            if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
            {
                return "";
            }
            std::string t1 = ResolveExpressionType(consequence, scope, symbolTable, sourceCode, uri);
            std::string t2 = ResolveExpressionType(alternative, scope, symbolTable, sourceCode, uri);
            if (t1 == t2)
            {
                return t1;
            }
            if (t1.empty()) return t2;
            if (t2.empty()) return t1;

            std::string c1 = CleanBaseType(t1);
            std::string c2 = CleanBaseType(t2);

            if (c1 == "double" || c2 == "double") return "double";
            if (c1 == "float" || c2 == "float") return "float";
            if (c1 == "int64" || c2 == "int64") return "int64";
            if (c1 == "uint64" || c2 == "uint64") return "uint64";
            if (c1 == "uint" || c2 == "uint") return "uint";
            if (IsPrimitiveTypeName(c1) && IsPrimitiveTypeName(c2)) return "int";

            // Inheritance check (derived vs base)
            auto h1 = GetInheritedTypeHierarchy(c1, symbolTable);
            for (const auto &b : h1)
            {
                if (CleanBaseType(b) == c2) return t2;
            }
            auto h2 = GetInheritedTypeHierarchy(c2, symbolTable);
            for (const auto &b : h2)
            {
                if (CleanBaseType(b) == c1) return t1;
            }

            return t1;
        }

        // Member expression (e.g. obj.member)
        if (nodeType == "member_expression")
        {
            TSNode objNode = ts_node_child_by_field_name(exprNode, "object", 6);
            TSNode memNode = ts_node_child_by_field_name(exprNode, "member", 6);
            if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
            {
                return "";
            }

            std::string objType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri);
            std::string cleanObj = CleanBaseType(objType);
            if (cleanObj.empty())
            {
                return "";
            }

            std::string memName = GetNodeText(memNode, sourceCode);
            while (!memName.empty() && isspace(static_cast<unsigned char>(memName.front()))) memName.erase(memName.begin());
            while (!memName.empty() && isspace(static_cast<unsigned char>(memName.back()))) memName.pop_back();
            auto hierarchy = GetInheritedTypeHierarchy(cleanObj, symbolTable);
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
                        return CleanExpressionType(sym.GetVariable().typeName);
                    }
                    else if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                    {
                        return CleanExpressionType(sym.GetFunction().returnType);
                    }
                }
            }
            return "";
        }

        // Call expression (e.g. func(arg1, arg2) or obj.method(arg1, arg2))
        if (nodeType == "call_expression")
        {
            TSNode funcNode = ts_node_child_by_field_name(exprNode, "function", 8);
            if (ts_node_is_null(funcNode) && ts_node_child_count(exprNode) > 0)
            {
                funcNode = ts_node_child(exprNode, 0);
            }
            if (ts_node_is_null(funcNode))
            {
                return "";
            }

            std::vector<std::string> argTypes;
            TSNode argsNode = ts_node_child_by_field_name(exprNode, "arguments", 9);
            if (!ts_node_is_null(argsNode))
            {
                uint32_t count = ts_node_named_child_count(argsNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode argChild = ts_node_named_child(argsNode, i);
                    argTypes.push_back(ResolveExpressionType(argChild, scope, symbolTable, sourceCode, uri));
                }
            }

            std::string_view funcNodeType = ts_node_type(funcNode);
            if (funcNodeType == "member_expression")
            {
                TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
                TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);
                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    std::string objType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri);
                    auto templateInfo = ParseTemplateType(objType);
                    std::string memName = GetNodeText(memNode, sourceCode);
                    while (!memName.empty() && isspace(static_cast<unsigned char>(memName.front()))) memName.erase(memName.begin());
                    while (!memName.empty() && isspace(static_cast<unsigned char>(memName.back()))) memName.pop_back();

                    if (templateInfo.containerName == "array" && !templateInfo.templateArgs.empty())
                    {
                        if (memName == "length" || memName == "size") { return "uint"; }
                        if (memName == "isEmpty") { return "bool"; }
                    }

                    std::string cleanObj = CleanBaseType(objType);
                    std::vector<Symbol> candidates;
                    auto hierarchy = GetInheritedTypeHierarchy(cleanObj, symbolTable);
                    for (const auto &typeName : hierarchy)
                    {
                        auto found = symbolTable.FindSymbols(typeName + "::" + memName);
                        for (const auto &sym : found)
                        {
                            if (sym.type == SymbolType::Function)
                            {
                                candidates.push_back(sym);
                            }
                        }
                    }

                    if (!candidates.empty())
                    {
                        auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
                        if (match.bestCandidate && std::holds_alternative<FunctionSignature>(match.bestCandidate->signature))
                        {
                            std::string ret = match.bestCandidate->GetFunction().returnType;
                            if (ret == "T" && !templateInfo.templateArgs.empty())
                            {
                                return templateInfo.templateArgs[0];
                            }
                            return CleanExpressionType(ret);
                        }
                        return CleanExpressionType(candidates[0].GetFunction().returnType);
                    }
                }
            }
            else
            {
                std::string funcName = GetNodeText(funcNode, sourceCode);
                std::vector<Symbol> inScope = FindSymbolsInScope(funcName, exprNode, sourceCode, symbolTable);
                std::vector<Symbol> candidates;
                for (const auto &sym : inScope)
                {
                    if (sym.type == SymbolType::Function)
                    {
                        candidates.push_back(sym);
                    }
                }
                if (!candidates.empty())
                {
                    auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
                    if (match.bestCandidate && std::holds_alternative<FunctionSignature>(match.bestCandidate->signature))
                    {
                        return CleanExpressionType(match.bestCandidate->GetFunction().returnType);
                    }
                    return CleanExpressionType(candidates[0].GetFunction().returnType);
                }
            }

            return ResolveExpressionType(funcNode, scope, symbolTable, sourceCode, uri);
        }

        // Cast expression (e.g. cast<Player@>(ent))
        if (nodeType == "cast_expression" || nodeType == "functional_cast_expression")
        {
            TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", 4);
            return ts_node_is_null(typeNode) ? std::string()
                                             : CleanExpressionType(GetNodeText(typeNode, sourceCode));
        }

        // Construct call expression (e.g. array<int>(5))
        if (nodeType == "construct_call_expression")
        {
            TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", 4);
            if (ts_node_is_null(typeNode))
            {
                return "";
            }

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
            return CleanExpressionType(written);
        }

        // Index expression (e.g. arr[i] or dict["key"])
        if (nodeType == "index_expression")
        {
            TSNode objNode = ts_node_child_by_field_name(exprNode, "object", 6);
            if (ts_node_is_null(objNode))
            {
                return "";
            }

            std::string objType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri);
            if (objType.empty())
            {
                return "";
            }

            auto templateInfo = ParseTemplateType(objType);
            if (templateInfo.containerName == "array" && !templateInfo.templateArgs.empty())
            {
                return CleanExpressionType(templateInfo.templateArgs[0]);
            }
            if ((templateInfo.containerName == "dictionary" || templateInfo.containerName == "map") &&
                templateInfo.templateArgs.size() >= 2)
            {
                return CleanExpressionType(templateInfo.templateArgs[1]);
            }

            std::string cleanObj = CleanBaseType(objType);
            std::vector<Symbol> candidates;
            auto hierarchy = GetInheritedTypeHierarchy(cleanObj, symbolTable);
            for (const auto &typeName : hierarchy)
            {
                auto found = symbolTable.FindSymbols(typeName + "::opIndex");
                for (const auto &sym : found)
                {
                    if (sym.type == SymbolType::Function)
                    {
                        candidates.push_back(sym);
                    }
                }
            }
            if (!candidates.empty())
            {
                return CleanExpressionType(candidates[0].GetFunction().returnType);
            }

            return "";
        }

        // Unary expression (e.g. !x, -x, ++i)
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

        // Postfix expression (e.g. i++)
        if (nodeType == "postfix_expression")
        {
            TSNode operandNode = ts_node_child_by_field_name(exprNode, "operand", 7);
            return ts_node_is_null(operandNode)
                       ? std::string()
                       : ResolveExpressionType(operandNode, scope, symbolTable, sourceCode, uri);
        }

        // Parenthesized expression (e.g. (expr))
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


