#include "analysis/SemanticHelpers.h"
#include "analysis/ASTUtils.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticContext.h"
#include "utils/Utils.h"
#include "parser/Keywords.h"

#include <optional>

namespace angel_lsp::analysis
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
        while (!typeName.empty() && (typeName.back() == '&' || typeName.back() == ' ' || typeName.back() == '\t'))
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

    bool IsReservedKeyword(const std::string &name)
    {
        // The list this used to hold was measured against the compiler word by word and came back
        // exactly right - 53, no additions, no removals. It moved to parser/Keywords.h so the
        // formatter and completion could stop keeping their own, differing, copies.
        return parser::keywords::IsReserved(name);
    }

    bool IsPrimitiveTypeName(const std::string &name)
    {
        return IsCorePrimitive(name);
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

    bool IsDestructorSymbol(const Symbol &sym)
    {
        if (sym.type != SymbolType::Function)
        {
            return false;
        }
        return sym.name.starts_with("~");
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

    bool NamesAFunctionNotAType(std::string_view name, const SymbolTable &table)
    {
        if (name.empty())
        {
            return false;
        }

        const auto symsPtr = table.FindSymbolsPtr(std::string(name));
        if (!symsPtr)
        {
            // Resolves to nothing. That is an unresolved type, assumed engine-registered, and not
            // this rule's business - see the header.
            return false;
        }

        bool isFunction = false;
        for (const auto &sym : *symsPtr)
        {
            switch (sym.type)
            {
            case SymbolType::Function:
                // A method is reached through its container and cannot be written bare in a type
                // position, so it says nothing about what the user meant here.
                if (sym.containerName.empty())
                {
                    isFunction = true;
                }
                break;

            // Anything that CAN stand in a type position settles it: the name is a type, whatever
            // else it also is. Funcdef in particular - a funcdef and a function may share a name,
            // and then `Foo@` is already valid.
            case SymbolType::Class:
            case SymbolType::Interface:
            case SymbolType::Enum:
            case SymbolType::Typedef:
            case SymbolType::Funcdef:
                return false;

            default:
                break;
            }
        }

        return isFunction;
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
        if (IsCorePrimitive(baseName)) return true;
        if (!ctx.request.GetStringTypeName().empty() && baseName == ctx.request.GetStringTypeName()) return true;
        if (!ctx.request.GetArrayTypeName().empty() && baseName == ctx.request.GetArrayTypeName()) return true;
        if (ctx.request.IsRegisteredSymbol(baseName)) return true;
        if (ctx.request.symbolTable.HasSymbolAnywhere(baseName)) return true;
        return false;
    }

    std::vector<std::string> SplitTemplateArguments(std::string_view inner)
    {
        std::vector<std::string> arguments;
        int depth = 0;
        size_t start = 0;

        const auto push = [&](std::string_view piece)
        {
            while (!piece.empty() && (piece.front() == ' ' || piece.front() == '	')) piece.remove_prefix(1);
            while (!piece.empty() && (piece.back() == ' ' || piece.back() == '	')) piece.remove_suffix(1);
            arguments.emplace_back(piece);
        };

        for (size_t i = 0; i < inner.size(); ++i)
        {
            if (inner[i] == '<')
                ++depth;
            else if (inner[i] == '>')
                --depth;
            else if (inner[i] == ',' && depth == 0)
            {
                push(inner.substr(start, i - start));
                start = i + 1;
            }
        }

        if (start <= inner.size())
            push(inner.substr(start));

        return arguments;
    }

    std::string LastScopeSegment(const std::string &name)
    {
        const size_t pos = name.rfind("::");
        return pos == std::string::npos ? name : name.substr(pos + 2);
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
            if (result.ends_with(" const"))
            {
                result.resize(result.size() - 6);
                modified = true;
            }
            else if (result.ends_with("[]"))
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

    std::string CanonicalizeArrayType(std::string_view typeName, std::string_view arrayTypeName)
    {
        std::string s(typeName);

        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '@' || s.back() == '&')) s.pop_back();

        if (s.starts_with("const "))
        {
            s = s.substr(6);
        }

        if (arrayTypeName.empty())
        {
            return s;
        }

        // The element of `int[][]` is `int[]`, so the element is canonicalised before it is
        // wrapped: `array<array<int>>`. Wrapping first would give `array<int[]>`, which is the
        // same type spelled in a way nothing else here recognises.
        while (s.ends_with("[]"))
        {
            const std::string element = CanonicalizeArrayType(s.substr(0, s.size() - 2), arrayTypeName);
            s = std::string(arrayTypeName) + "<" + element + ">";
        }

        return s;
    }

    std::string MemberOwnerType(std::string_view typeName, std::string_view arrayTypeName)
    {
        const std::string canonical = CanonicalizeArrayType(typeName, arrayTypeName);

        // A template instantiation's members are declared on the template, so `array<int>` reaches
        // `array::length`. Split on the *first* `<`, which is also the outermost one.
        if (canonical.ends_with(">"))
        {
            const size_t open = canonical.find('<');
            if (open != std::string::npos && open > 0)
            {
                std::string container = canonical.substr(0, open);
                while (!container.empty() && (container.back() == ' ' || container.back() == '\t'))
                {
                    container.pop_back();
                }
                if (!container.empty())
                {
                    return container;
                }
            }
        }

        return CleanBaseType(canonical);
    }

    std::string SubstituteTypeParam(std::string_view typeStr, std::string_view paramName, std::string_view concreteType)
    {
        if (typeStr.empty() || paramName.empty())
        {
            return std::string(typeStr);
        }
        std::string result;
        size_t i = 0;
        while (i < typeStr.size())
        {
            if (typeStr.substr(i).starts_with(paramName))
            {
                bool leftBoundary = (i == 0) || (!isalnum(static_cast<unsigned char>(typeStr[i - 1])) && typeStr[i - 1] != '_');
                size_t nextIdx = i + paramName.size();
                bool rightBoundary = (nextIdx >= typeStr.size()) || (!isalnum(static_cast<unsigned char>(typeStr[nextIdx])) && typeStr[nextIdx] != '_');
                if (leftBoundary && rightBoundary)
                {
                    result.append(concreteType);
                    i = nextIdx;
                    continue;
                }
            }
            result.push_back(typeStr[i]);
            ++i;
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

        // Empty pieces are dropped here: `array<int,>` names one argument, and the trailing
        // nothing is a typo rather than a second one.
        for (std::string &argument : SplitTemplateArguments(inner))
        {
            if (!argument.empty())
                info.templateArgs.push_back(std::move(argument));
        }

        return info;
    }

    std::string PropertyNameFromAccessor(const Symbol &sym, bool keywordRequired)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
            return {};

        if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
            return {};

        for (const std::string_view prefix : { std::string_view("get_"), std::string_view("set_") })
        {
            if (sym.name.size() > prefix.size() && sym.name.compare(0, prefix.size(), prefix) == 0)
                return sym.name.substr(prefix.size());
        }

        return {};
    }

    std::vector<Symbol> FindPropertyAccessors(const std::string &typeName,
                                              const std::string &propertyName,
                                              const SymbolTable &symbolTable,
                                              bool keywordRequired)
    {
        std::vector<Symbol> accessors;
        if (typeName.empty() || propertyName.empty())
            return accessors;

        // Getter first, and the whole hierarchy before moving on to the setter, so the type this
        // reports comes from the getter whenever there is one.
        for (const std::string_view prefix : { std::string_view("get_"), std::string_view("set_") })
        {
            for (const auto &owner : GetInheritedTypeHierarchy(typeName, symbolTable))
            {
                for (const auto &sym : symbolTable.FindSymbols(owner + "::" + std::string(prefix) + propertyName))
                {
                    if (!std::holds_alternative<FunctionSignature>(sym.signature))
                        continue;
                    if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
                        continue;

                    accessors.push_back(sym);
                }

                // A derived class overriding the accessor answers for it; the base's copy would only
                // be the same property said twice.
                if (!accessors.empty())
                    break;
            }
        }

        return accessors;
    }

    std::string PropertyTypeFromAccessors(const std::vector<Symbol> &accessors)
    {
        for (const auto &sym : accessors)
        {
            if (!std::holds_alternative<FunctionSignature>(sym.signature))
                continue;

            const FunctionSignature &func = sym.GetFunction();
            if (sym.name.compare(0, 4, "get_") == 0)
                return func.returnType;
        }

        for (const auto &sym : accessors)
        {
            if (!std::holds_alternative<FunctionSignature>(sym.signature))
                continue;

            const FunctionSignature &func = sym.GetFunction();
            if (sym.name.compare(0, 4, "set_") == 0 && !func.parameters.empty())
                return func.parameters.front().typeName;
        }

        return {};
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

            // FindSymbolsPtr, not FindSymbols: the latter deep-copies the whole overload bucket,
            // and Symbol is a heavy value type. Nothing here mutates it.
            const auto symbols = symbolTable.FindSymbolsPtr(curType);
            for (const auto &sym : (symbols ? *symbols : std::vector<Symbol>{}))
            {
                if (sym.type == SymbolType::Class)
                {
                    const auto &cls = sym.GetClass();

                    // Classified once, then partitioned. Mixins must still be enqueued ahead of
                    // ordinary bases - that ordering is what makes a mixin's method take precedence
                    // - but the two passes this used to take each re-cleaned every base name and
                    // re-probed the symbol table through IsMixinClass, so every base was resolved
                    // twice to answer the same question.
                    struct ClassifiedBase
                    {
                        std::string name;
                        bool isMixin = false;
                    };

                    std::vector<ClassifiedBase> bases;
                    bases.reserve(cls.bases.size());

                    for (const auto &base : cls.bases)
                    {
                        std::string cleanBase = CleanBaseType(base);
                        if (cleanBase.empty())
                        {
                            continue;
                        }
                        const bool isMixin = IsMixinClass(cleanBase, symbolTable);
                        bases.push_back(ClassifiedBase{ std::move(cleanBase), isMixin });
                    }

                    for (const bool wantMixins : { true, false })
                    {
                        for (const auto &base : bases)
                        {
                            if (base.isMixin != wantMixins)
                            {
                                continue;
                            }
                            if (visited.insert(base.name).second)
                            {
                                queue.push_back(base.name);
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

    bool HierarchyIsFullyVisible(const std::string &typeName, const SymbolTable &symbolTable)
    {
        for (const auto &ancestor : GetInheritedTypeHierarchy(typeName, symbolTable))
        {
            const auto symbols = symbolTable.FindSymbolsPtr(ancestor);
            if (!symbols)
            {
                return false;
            }

            // The hierarchy walk itself stops at a name it cannot resolve, so an unresolvable base
            // never appears in the list above and has to be looked for here, one level down.
            for (const auto &sym : *symbols)
            {
                if (sym.type != SymbolType::Class)
                {
                    continue;
                }
                for (const auto &base : sym.GetClass().bases)
                {
                    if (!symbolTable.FindSymbolsPtr(CleanBaseType(base)))
                    {
                        return false;
                    }
                }
            }
        }
        return true;
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

        // 2. Discover derived classes, breadth-first over the reverse inheritance edges.
        //
        // This used to be a fixpoint loop whose every iteration walked the entire symbol table,
        // repeating until no new derived class turned up - O(inheritance depth x whole workspace)
        // per call. Inheritance is written the wrong way round for this question (a class names its
        // bases, not its children), so the index reverses the edges once per table version and the
        // search becomes an ordinary traversal. See RuleIndex::derivedByBase.
        const auto ruleIndex = symbolTable.GetRuleIndex();

        std::vector<std::string> frontier(related.begin(), related.end());
        while (!frontier.empty())
        {
            std::vector<std::string> next;

            for (const auto &baseName : frontier)
            {
                const auto it = ruleIndex->derivedByBase.find(baseName);
                if (it == ruleIndex->derivedByBase.end())
                {
                    continue;
                }

                for (const auto &derived : it->second)
                {
                    if (!visited.insert(derived.qualifiedName).second)
                    {
                        continue;
                    }

                    related.push_back(derived.qualifiedName);
                    next.push_back(derived.qualifiedName);

                    // The unqualified spelling is recorded too - callers look types up by either.
                    if (!derived.name.empty() && derived.name != derived.qualifiedName &&
                        visited.insert(derived.name).second)
                    {
                        related.push_back(derived.name);
                        next.push_back(derived.name);
                    }
                }
            }

            frontier = std::move(next);
        }

        return related;
    }

    bool IsBaseConstructorCall(TSNode node, std::string_view sourceCode)
    {
        if (ts_node_is_null(node))
        {
            return false;
        }

        // Walk out to the function this sits in. The cap is the same one the rest of the analyzer
        // uses for ancestor walks; a tree deeper than that is malformed, not merely nested.
        TSNode function = ts_node_parent(node);
        int depth = 0;
        while (!ts_node_is_null(function) && ts_node_type(function) != std::string_view("func_declaration"))
        {
            if (++depth > k_maxAstDepth)
            {
                return false;
            }
            function = ts_node_parent(function);
        }

        if (ts_node_is_null(function))
        {
            return false;
        }

        // A constructor is a func_declaration with no return type whose name is the class's. The
        // absence of the field is what distinguishes it - `void Derived()` is a method that happens
        // to share the name and cannot call super.
        if (!ts_node_is_null(ts_node_child_by_field_name(function, "return_type", 11)))
        {
            return false;
        }

        TSNode functionName = ts_node_child_by_field_name(function, "name", 4);
        if (ts_node_is_null(functionName))
        {
            return false;
        }

        TSNode owner = ts_node_parent(function);
        depth = 0;
        while (!ts_node_is_null(owner) && ts_node_type(owner) != std::string_view("class_declaration"))
        {
            if (++depth > k_maxAstDepth)
            {
                return false;
            }
            owner = ts_node_parent(owner);
        }

        if (ts_node_is_null(owner))
        {
            return false;
        }

        TSNode className = ts_node_child_by_field_name(owner, "name", 4);
        if (ts_node_is_null(className) ||
            GetNodeText(className, sourceCode) != GetNodeText(functionName, sourceCode))
        {
            return false;
        }

        // And the class has to have a base to call up into. `super(...)` in a class that names none
        // is an error the compiler does report, so this stops short of excusing it.
        const uint32_t childCount = ts_node_named_child_count(owner);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            if (ts_node_type(ts_node_named_child(owner, i)) == std::string_view("base_class_list"))
            {
                return true;
            }
        }

        return false;
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

    std::vector<std::string> CollectUsingNamespaces(TSNode root, std::string_view sourceCode)
    {
        std::vector<std::string> usings;
        std::vector<TSNode> stack = { root };
        while (!stack.empty())
        {
            TSNode cur = stack.back();
            stack.pop_back();

            std::string_view type = ts_node_type(cur);
            if (type == "using_declaration")
            {
                TSNode nameNode = ts_node_child_by_field_name(cur, "name", 4);
                if (!ts_node_is_null(nameNode))
                {
                    std::string uName = GetNodeText(nameNode, sourceCode);
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front()))) uName.erase(uName.begin());
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back()))) uName.pop_back();
                    if (!uName.empty())
                    {
                        usings.push_back(uName);
                    }
                }
            }

            uint32_t count = ts_node_child_count(cur);
            for (uint32_t i = 0; i < count; ++i)
            {
                stack.push_back(ts_node_child(cur, i));
            }
        }
        return usings;
    }

    bool IsKnownScope(const std::string &prefix, TSNode node, std::string_view sourceCode, const SymbolTable &table)
    {
        if (prefix.empty())
        {
            return true;
        }

        // 1. Check if prefix is directly in table as symbol or qualified name
        if (table.HasSymbol(prefix) || table.HasSymbolAnywhere(prefix))
        {
            return true;
        }

        // 2. Check if any symbol in table has containerName equal to or prefixed with prefix
        bool hasContainer = false;
        table.ForEachSymbol([&](const std::string &qName, const std::vector<Symbol> &syms) {
            if (hasContainer) return;
            for (const auto &s : syms)
            {
                if (s.containerName == prefix || s.containerName.rfind(prefix + "::", 0) == 0 ||
                    s.name == prefix || qName == prefix || qName.rfind(prefix + "::", 0) == 0)
                {
                    hasContainer = true;
                    return;
                }
            }
        });
        if (hasContainer)
        {
            return true;
        }

        // 3. Check relative to enclosing containers
        auto containers = GetEnclosingContainers(node, sourceCode);
        for (const auto &c : containers)
        {
            std::string q = c.qualifiedName + "::" + prefix;
            if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
            {
                return true;
            }
        }

        // 4. Check relative to using namespaces
        TSNode root = node;
        while (!ts_node_is_null(ts_node_parent(root)))
        {
            root = ts_node_parent(root);
        }
        auto usings = CollectUsingNamespaces(root, sourceCode);
        for (const auto &ns : usings)
        {
            std::string q = ns + "::" + prefix;
            if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
            {
                return true;
            }
        }

        return false;
    }

    std::vector<Symbol> FindSymbolsInScope(
        const SymbolTable &symbolTable,
        const std::vector<ContainerInfo> &containers,
        const std::string &name,
        const std::vector<std::string> &usingNamespaces)
    {
        if (name.empty())
        {
            return {};
        }

        // Handle explicit global scope operator "::name"
        if (name.rfind("::", 0) == 0)
        {
            std::string globalName = name.substr(2);
            return symbolTable.FindSymbols(globalName);
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

        // 3. using namespace lookup
        for (const auto &ns : usingNamespaces)
        {
            std::string qName = ns + "::" + name;
            auto found = symbolTable.FindSymbols(qName);
            if (!found.empty())
            {
                return found;
            }
        }

        // 4. Enum member fallback
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
        std::vector<std::string> usings;
        TSNode p = node;
        while (!ts_node_is_null(p))
        {
            uint32_t count = ts_node_child_count(p);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode ch = ts_node_child(p, i);
                if (std::string_view(ts_node_type(ch)) == "using_declaration")
                {
                    TSNode nameNode = ts_node_child_by_field_name(ch, "name", 4);
                    if (!ts_node_is_null(nameNode))
                    {
                        std::string uName = GetNodeText(nameNode, sourceCode);
                        while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front()))) uName.erase(uName.begin());
                        while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back()))) uName.pop_back();
                        if (!uName.empty())
                        {
                            usings.push_back(uName);
                        }
                    }
                }
            }
            p = ts_node_parent(p);
        }

        std::vector<ContainerInfo> containers = GetEnclosingContainers(node, sourceCode);
        return FindSymbolsInScope(symbolTable, containers, name, usings);
    }

    std::string ResolveExpressionType(
        TSNode exprNode,
        const Scope *scope,
        const SymbolTable &symbolTable,
        std::string_view sourceCode,
        std::string_view uri,
        int depth)
    {
        // This resolver recurses on operands and member chains with nothing else bounding it, so a
        // deeply nested expression walked the stack down until it ran out. Returning empty is the
        // established "cannot see enough to judge" answer everywhere in this file, and every caller
        // already treats it as "stay silent" - so a truncated resolution costs a diagnostic, never
        // a wrong one. See k_maxAstDepth in ASTUtils.h.
        if (depth > k_maxAstDepth)
        {
            return "";
        }

        if (ts_node_is_null(exprNode) || sourceCode.empty())
        {
            return "";
        }

        std::string rawText = GetNodeText(exprNode, sourceCode);
        while (!rawText.empty() && isspace(static_cast<unsigned char>(rawText.front()))) rawText.erase(rawText.begin());
        while (!rawText.empty() && isspace(static_cast<unsigned char>(rawText.back()))) rawText.pop_back();

        if (rawText == "void")
        {
            return "void";
        }

        if (!rawText.empty() && rawText.front() == '{' && rawText.back() == '}')
        {
            return "init_list";
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

            // `d`, `e` and `f` are hex digits, so a hex literal must never be scanned for the
            // float suffix and the exponent marker: `0xefc60000` is a uint, and reading it as a
            // float made every rule downstream believe a bitwise expression was floating point.
            // Found by the numeric-warning corpus audit, which reported a truncation on
            // `y ^= (y << 15) & 0xefc60000;` in a Mersenne twister. `u` and `l` are not hex
            // digits, so the width suffixes below stay meaningful either way.
            const bool isRadixPrefixed = text.size() > 1 && text[0] == '0' &&
                                         (text[1] == 'x' || text[1] == 'X' ||
                                          text[1] == 'b' || text[1] == 'B');

            if (!isRadixPrefixed)
            {
                if (text.find('f') != std::string::npos || text.find('F') != std::string::npos)
                {
                    return "float";
                }
                if (text.find('.') != std::string::npos || text.find('e') != std::string::npos || text.find('E') != std::string::npos)
                {
                    return "double";
                }
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
                       : ResolveExpressionType(lastIdentifier, scope, symbolTable, sourceCode, uri, depth + 1);
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
                return ResolveExpressionType(left, scope, symbolTable, sourceCode, uri, depth + 1);
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
            std::string leftType = ResolveExpressionType(left, scope, symbolTable, sourceCode, uri, depth + 1);
            std::string rightType = ResolveExpressionType(right, scope, symbolTable, sourceCode, uri, depth + 1);

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
                if ((op == "|" || op == "&" || op == "^") && cleanLeft == cleanRight)
                {
                    return cleanLeft;
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
            std::string t1 = ResolveExpressionType(consequence, scope, symbolTable, sourceCode, uri, depth + 1);
            std::string t2 = ResolveExpressionType(alternative, scope, symbolTable, sourceCode, uri, depth + 1);
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

            std::string objType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri, depth + 1);
            std::string cleanObj = CleanBaseType(objType);
            if (cleanObj.empty())
            {
                return "";
            }

            std::string memName = GetNodeText(memNode, sourceCode);
            while (!memName.empty() && isspace(static_cast<unsigned char>(memName.front()))) memName.erase(memName.begin());
            while (!memName.empty() && isspace(static_cast<unsigned char>(memName.back()))) memName.pop_back();
            auto hierarchy = GetInheritedTypeHierarchy(cleanObj, symbolTable);
            std::vector<std::string> propSearchOrder;
            if (!hierarchy.empty())
            {
                propSearchOrder.push_back(hierarchy[0]);
                for (size_t i = 1; i < hierarchy.size(); ++i)
                {
                    if (!IsMixinClass(hierarchy[i], symbolTable))
                    {
                        propSearchOrder.push_back(hierarchy[i]);
                    }
                }
                for (size_t i = 1; i < hierarchy.size(); ++i)
                {
                    if (IsMixinClass(hierarchy[i], symbolTable))
                    {
                        propSearchOrder.push_back(hierarchy[i]);
                    }
                }
            }

            for (const auto &typeName : propSearchOrder)
            {
                std::string qName = typeName + "::" + memName;
                auto found = symbolTable.FindSymbols(qName);
                if (found.empty())
                {
                    found = symbolTable.FindSymbols(typeName + "::get_" + memName);
                }
                if (found.empty())
                {
                    found = symbolTable.FindSymbols(typeName + "::set_" + memName);
                }
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
                    else if (sym.type == SymbolType::Function)
                    {
                        if (!sym.GetFunction().returnType.empty() && sym.GetFunction().returnType != "void")
                        {
                            return CleanExpressionType(sym.GetFunction().returnType);
                        }
                        else if (!sym.GetFunction().parameters.empty())
                        {
                            return CleanExpressionType(sym.GetFunction().parameters.back().typeName);
                        }
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
                    argTypes.push_back(ResolveExpressionType(argChild, scope, symbolTable, sourceCode, uri, depth + 1));
                }
            }

            std::string_view funcNodeType = ts_node_type(funcNode);
            if (funcNodeType == "member_expression")
            {
                TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
                TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);
                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    // Canonicalised before it is parsed as a template: `int[]` and `array<int>` are
                    // the same type, and only the second was ever recognised here, so `a.length()`
                    // on a bracket-declared array resolved to nothing at all.
                    std::string objType = CanonicalizeArrayType(
                        ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri, depth + 1));
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

            std::string calleeType = ResolveExpressionType(funcNode, scope, symbolTable, sourceCode, uri, depth + 1);
            std::string cleanCallee = CleanBaseType(calleeType);
            if (!cleanCallee.empty())
            {
                auto funcdefSymbols = symbolTable.FindSymbols(cleanCallee);
                for (const auto &s : funcdefSymbols)
                {
                    if (s.type == SymbolType::Funcdef)
                    {
                        return CleanExpressionType(s.GetFuncdef().returnType);
                    }
                }
            }

            return calleeType;
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

            std::string objType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri, depth + 1);
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

                // `@f` where `f` names a function is a *function handle*, and which funcdef it
                // becomes is decided by what it is being passed to - AngelScript picks the one the
                // parameter asks for. There is no single answer to give here, so the honest one is
                // none: resolving through to the operand returned `f`'s return type instead, and
                // `createCoRoutine(@worker, args)` was reported as "Cannot implicitly convert
                // 'void' to 'coroutine@'" on correct code.
                if (op == "@")
                {
                    TSNode target = ts_node_child_by_field_name(exprNode, "operand", 7);
                    if (!ts_node_is_null(target))
                    {
                        std::string targetName = GetNodeText(target, sourceCode);
                        while (!targetName.empty() && isspace(static_cast<unsigned char>(targetName.front()))) targetName.erase(targetName.begin());
                        while (!targetName.empty() && isspace(static_cast<unsigned char>(targetName.back()))) targetName.pop_back();

                        if (!targetName.empty() && targetName.find('(') == std::string::npos)
                        {
                            bool namesFunction = false;
                            if (const auto bucket = symbolTable.FindSymbolsPtr(targetName))
                            {
                                for (const auto &candidate : *bucket)
                                {
                                    if (candidate.type == SymbolType::Function)
                                    {
                                        namesFunction = true;
                                        break;
                                    }
                                }
                            }
                            if (namesFunction)
                            {
                                return {};
                            }
                        }
                    }
                }
            }

            TSNode operandNode = ts_node_child_by_field_name(exprNode, "operand", 7);
            return ts_node_is_null(operandNode)
                       ? std::string()
                       : ResolveExpressionType(operandNode, scope, symbolTable, sourceCode, uri, depth + 1);
        }

        // Postfix expression (e.g. i++)
        if (nodeType == "postfix_expression")
        {
            TSNode operandNode = ts_node_child_by_field_name(exprNode, "operand", 7);
            return ts_node_is_null(operandNode)
                       ? std::string()
                       : ResolveExpressionType(operandNode, scope, symbolTable, sourceCode, uri, depth + 1);
        }

        // Parenthesized expression (e.g. (expr))
        if (nodeType == "parenthesized_expression")
        {
            if (ts_node_named_child_count(exprNode) > 0)
            {
                return ResolveExpressionType(ts_node_named_child(exprNode, 0), scope, symbolTable, sourceCode, uri, depth + 1);
            }
            uint32_t count = ts_node_child_count(exprNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(exprNode, i);
                std::string_view cType = ts_node_type(child);
                if (cType != "(" && cType != ")")
                {
                    return ResolveExpressionType(child, scope, symbolTable, sourceCode, uri, depth + 1);
                }
            }
            return "";
        }

        // Initializer list (e.g. {123}, {"s"})
        if (nodeType == "initializer_list")
        {
            uint32_t count = ts_node_named_child_count(exprNode);
            if (count > 0)
            {
                std::string elemType = ResolveExpressionType(ts_node_named_child(exprNode, 0), scope, symbolTable, sourceCode, uri, depth + 1);
                return "{" + elemType + "}";
            }
            return "{}";
        }

        return "";
    }

    bool IsVariableType(std::string_view typeName)
    {
        // Strip whatever decoration the declaration carried: const, &, @, in/out/inout, spaces.
        // What has to remain is a bare '?' and nothing else.
        std::string cleaned;
        cleaned.reserve(typeName.size());
        for (const char c : typeName)
        {
            if (c == '?')
                cleaned.push_back(c);
            else if (c != ' ' && c != '\t' && c != '&' && c != '@')
            {
                // Any other identifier character means this is a real type, not the wildcard.
                // "const" / "in" / "out" / "inout" are the only words legally adjacent to it.
                cleaned.push_back(c);
            }
        }

        static constexpr std::string_view k_decorations[] = { "const", "inout", "out", "in" };
        for (const auto decoration : k_decorations)
        {
            for (size_t at = cleaned.find(decoration); at != std::string::npos; at = cleaned.find(decoration, at))
                cleaned.erase(at, decoration.size());
        }

        return cleaned == "?";
    }

    // --- A lambda against the funcdef it is being handed to -------------------------------

    namespace
    {
        /** @brief `int32` and `uint32` are the explicit spellings of `int` and `uint`. */
        std::string_view CanonicalPrimitiveSpelling(std::string_view typeName) noexcept
        {
            if (typeName == "int32") return "int";
            if (typeName == "uint32") return "uint";
            return typeName;
        }

        std::string LastSegmentOf(const std::string &name)
        {
            const size_t at = name.rfind("::");
            return at == std::string::npos ? name : name.substr(at + 2);
        }

        /** @brief True for a name that denotes a typedef, whose alias no spelling can see through. */
        bool NamesATypedef(const std::string &name, const SymbolTable &table)
        {
            const auto bucket = table.FindSymbolsPtr(name);
            if (!bucket)
            {
                return false;
            }
            for (const auto &sym : *bucket)
            {
                if (sym.type == SymbolType::Typedef)
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool IsLambdaExpression(TSNode node) noexcept
    {
        if (ts_node_is_null(node))
        {
            return false;
        }
        const std::string_view type(ts_node_type(node));
        return type == node_types::LambdaExpression;
    }

    std::vector<LambdaParameter> ReadLambdaParameters(TSNode listNode, std::string_view sourceCode)
    {
        std::vector<LambdaParameter> parameters;
        if (ts_node_is_null(listNode))
        {
            return parameters;
        }

        LambdaParameter current;
        bool groupHasContent = false;

        const uint32_t childCount = ts_node_child_count(listNode);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            TSNode child = ts_node_child(listNode, i);
            const std::string text = GetNodeText(child, sourceCode);

            if (text == "(")
            {
                continue;
            }
            if (text == ")")
            {
                break;
            }
            if (text == ",")
            {
                parameters.push_back(current);
                current = LambdaParameter{};
                groupHasContent = false;
                continue;
            }

            const char *field = ts_node_field_name_for_child(listNode, i);
            if (field && std::string_view(field) == "param_type")
            {
                current.hasWrittenType = true;
                current.isConst = text.starts_with("const ") || text == "const";
                current.isHandle = text.find('@') != std::string::npos;
                current.typeName = CleanBaseType(text);
            }
            else if (text == "&")
            {
                current.isReference = true;
            }
            else if (text == "in" || text == "out" || text == "inout")
            {
                current.modifier = text == "in"  ? ParameterModifier::In
                                 : text == "out" ? ParameterModifier::Out
                                                 : ParameterModifier::InOut;
            }

            groupHasContent = true;
        }

        if (groupHasContent)
        {
            parameters.push_back(current);
        }
        return parameters;
    }

    bool LambdaContradictsFuncdef(const std::vector<LambdaParameter> &lambdaParameters,
                                  const FuncdefSignature &funcdefSig,
                                  const SymbolTable &table)
    {
        if (lambdaParameters.size() != funcdefSig.parameters.size())
        {
            return true;
        }

        for (size_t i = 0; i < lambdaParameters.size(); ++i)
        {
            const LambdaParameter &written = lambdaParameters[i];
            if (!written.hasWrittenType)
            {
                continue;
            }

            const ParameterInformation &expected = funcdefSig.parameters[i];
            if (written.isHandle != expected.isHandle ||
                written.isReference != expected.isReference ||
                written.modifier != expected.modifier)
            {
                return true;
            }

            // Compared by last `::` segment, so a name the funcdef writes bare and the lambda
            // writes qualified - or the other way round - is the same name. That costs the
            // `A::Foo` against `B::Foo` case, which is a rejection this misses rather than a legal
            // program it reports.
            const std::string writtenBase(CanonicalPrimitiveSpelling(LastSegmentOf(written.typeName)));
            const std::string expectedBase(
                CanonicalPrimitiveSpelling(LastSegmentOf(CleanBaseType(expected.typeName))));

            if (writtenBase.empty() || expectedBase.empty() || writtenBase == expectedBase)
            {
                continue;
            }
            if (NamesATypedef(writtenBase, table) || NamesATypedef(expectedBase, table))
            {
                continue;
            }
            return true;
        }

        return false;
    }

    bool LambdaContradictsFuncdef(TSNode lambdaNode,
                                  const FuncdefSignature &funcdefSig,
                                  const SymbolTable &table,
                                  std::string_view sourceCode)
    {
        TSNode listNode = ts_node_child_by_field_name(lambdaNode, "parameters", 10);
        if (ts_node_is_null(listNode))
        {
            // No parameter list to read is not the same as an empty one, and guessing which it is
            // would be guessing about the whole signature.
            return false;
        }
        return LambdaContradictsFuncdef(ReadLambdaParameters(listNode, sourceCode), funcdefSig, table);
    }

    std::optional<Symbol> FindFuncdefSymbol(const std::string &typeName, const SymbolTable &table)
    {
        if (typeName.empty())
        {
            return std::nullopt;
        }

        if (const auto bucket = table.FindSymbolsPtr(typeName))
        {
            for (const auto &sym : *bucket)
            {
                if (sym.type == SymbolType::Funcdef)
                {
                    return sym;
                }
            }
        }

        const std::string bare = LastSegmentOf(typeName);
        std::optional<Symbol> found;
        table.ForEachSymbol([&](const std::string &qualifiedName, const std::vector<Symbol> &symbols)
        {
            if (found || (qualifiedName != bare && LastSegmentOf(qualifiedName) != bare))
            {
                return;
            }
            for (const auto &sym : symbols)
            {
                if (sym.type == SymbolType::Funcdef)
                {
                    found = sym;
                    return;
                }
            }
        });
        return found;
    }

    /**
     * @brief The funcdef a lambda is being handed to, read from where it is written.
     *
     * A lambda has no return type of its own - the grammar gives `lambda_expression` a parameter
     * list and a body and nothing else - so every question about what its body must return has to
     * come from the target. That is what this answers, for the three shapes a lambda reaches a
     * funcdef through:
     *
     *     CB@ cb = function(...) { };      a declaration whose written type is a funcdef
     *     Register(CB(function(...)));     a funcdef used as a conversion
     *     Take(function(...));             an argument landing on a funcdef parameter
     *
     * Anything else answers nullopt, and so does a call with more than one candidate offering a
     * different funcdef at that position: which one the lambda was written against is precisely
     * what is undecided there, and a return-type verdict drawn from a guess would be worse than
     * the silence it replaced.
     */
    std::optional<Symbol> FuncdefTargetOfLambda(TSNode lambdaNode,
                                                const SymbolTable &table,
                                                std::string_view sourceCode)
    {
        if (!IsLambdaExpression(lambdaNode))
        {
            return std::nullopt;
        }

        TSNode parent = ts_node_parent(lambdaNode);
        if (ts_node_is_null(parent))
        {
            return std::nullopt;
        }

        const std::string_view parentType(ts_node_type(parent));

        // `CB@ cb = function(...) { };` - the declaration writes the type down.
        if (parentType == "variable_declarator")
        {
            TSNode declaration = ts_node_parent(parent);
            if (!ts_node_is_null(declaration))
            {
                TSNode typeNode = ts_node_child_by_field_name(declaration, "var_type", 8);
                if (ts_node_is_null(typeNode))
                {
                    typeNode = ts_node_child_by_field_name(declaration, "type", 4);
                }
                if (!ts_node_is_null(typeNode))
                {
                    return FindFuncdefSymbol(CleanBaseType(GetNodeText(typeNode, sourceCode)), table);
                }
            }
            return std::nullopt;
        }

        if (parentType != "argument_list")
        {
            return std::nullopt;
        }

        // Which argument this lambda is, and what is being called.
        uint32_t position = 0;
        bool found = false;
        for (uint32_t i = 0; i < ts_node_named_child_count(parent); ++i)
        {
            if (ts_node_eq(ts_node_named_child(parent, i), lambdaNode))
            {
                position = i;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return std::nullopt;
        }

        TSNode call = ts_node_parent(parent);
        if (ts_node_is_null(call))
        {
            return std::nullopt;
        }

        TSNode callee = ts_node_child_by_field_name(call, "type", 4);
        if (ts_node_is_null(callee))
        {
            callee = ts_node_child_by_field_name(call, "function", 8);
        }
        if (ts_node_is_null(callee) && ts_node_child_count(call) > 0)
        {
            callee = ts_node_child(call, 0);
        }
        if (ts_node_is_null(callee))
        {
            return std::nullopt;
        }

        const std::string calleeName = CleanBaseType(GetNodeText(callee, sourceCode));

        // `Register(CB(function(...)))` - the callee IS the funcdef, used as a conversion.
        if (position == 0 && ts_node_named_child_count(parent) == 1)
        {
            if (auto asConversion = FindFuncdefSymbol(calleeName, table))
            {
                return asConversion;
            }
        }

        // `Take(function(...))` - the parameter at this position names the funcdef. Every
        // candidate has to agree on which one, or there is nothing to be sure of.
        std::optional<Symbol> agreed;
        bool sawCandidate = false;
        const auto lookUp = [&](const std::string &name) -> bool
        {
            const auto bucket = table.FindSymbolsPtr(name);
            if (!bucket)
            {
                return false;
            }
            for (const auto &sym : *bucket)
            {
                if (sym.type != SymbolType::Function ||
                    !std::holds_alternative<FunctionSignature>(sym.signature))
                {
                    continue;
                }
                const auto &parameters = sym.GetFunction().parameters;
                if (position >= parameters.size())
                {
                    return false;
                }
                auto funcdef = FindFuncdefSymbol(CleanBaseType(parameters[position].typeName), table);
                if (!funcdef)
                {
                    return false;
                }
                if (sawCandidate && agreed && agreed->name != funcdef->name)
                {
                    return false;
                }
                agreed = std::move(funcdef);
                sawCandidate = true;
            }
            return sawCandidate;
        };

        const size_t lastSeparator = calleeName.rfind("::");
        const std::string memberName = calleeName.rfind('.') != std::string::npos
                                           ? calleeName.substr(calleeName.rfind('.') + 1)
                                           : calleeName;
        if (!lookUp(calleeName) && !lookUp(memberName) &&
            !(lastSeparator != std::string::npos && lookUp(calleeName.substr(lastSeparator + 2))))
        {
            return std::nullopt;
        }
        return agreed;
    }
}
