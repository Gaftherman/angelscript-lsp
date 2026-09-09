#include "analysis/InitializerListChecker.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>
#include "parser/GrammarNames.h"

namespace angel_lsp::analysis
{
    namespace
    {
        constexpr uint32_t k_varTypeFieldLength = 8;   ///< "var_type"
        constexpr uint32_t k_typeFieldLength = 4;      ///< "type"
        constexpr uint32_t k_valueFieldLength = 5;     ///< "value"

        std::string Trimmed(std::string text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.erase(text.begin());
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.pop_back();
            return text;
        }

        /**
         * @brief Strips the decorations that do not change which values a type accepts.
         *
         * `const`, `&` and `@` all leave the underlying type - and therefore its list pattern -
         * exactly as it was. The brackets and angle brackets are deliberately kept: those *are* the
         * shape this rule reads. That is also why `SemanticHelpers::CleanBaseType` cannot be used
         * here despite doing most of the same work - it strips `[]` and unwraps `array<...>` down
         * to the element type, which is precisely the information that has to survive.
         *
         * These three are language syntax rather than configuration: AngelScript spells them this
         * way and no host can change it.
         */
        std::string StripDecorations(std::string_view raw)
        {
            std::string text = Trimmed(std::string(raw));

            for (;;)
            {
                if (text.starts_with("const") && text.size() > 5 &&
                    std::isspace(static_cast<unsigned char>(text[5])))
                {
                    text = Trimmed(text.substr(5));
                    continue;
                }
                if (!text.empty() && (text.back() == '@' || text.back() == '&'))
                {
                    text.pop_back();
                    text = Trimmed(text);
                    continue;
                }
                // A trailing `const` is only a qualifier when something separates it from the type
                // name; without the boundary check a class called `Wconst` would be truncated to
                // `W` and then match nothing.
                if (text.size() > 5 && text.ends_with("const"))
                {
                    const char before = text[text.size() - 6];
                    if (std::isspace(static_cast<unsigned char>(before)) || before == '&' || before == '@')
                    {
                        text = Trimmed(text.substr(0, text.size() - 5));
                        continue;
                    }
                }
                break;
            }

            // Whitespace inside the shape - `array < int >` - would defeat the tests below, and
            // none of it is meaningful.
            std::string compact;
            compact.reserve(text.size());
            for (char c : text)
            {
                if (!std::isspace(static_cast<unsigned char>(c)))
                    compact += c;
            }
            return compact;
        }

        /** @brief `Name<A, B>` split into its name and its arguments; arguments empty if none. */
        struct TemplateSpelling
        {
            std::string name;
            std::vector<std::string> arguments;
        };

        TemplateSpelling ReadTemplateSpelling(const std::string &type)
        {
            TemplateSpelling spelling;

            const size_t open = type.find('<');
            if (open == std::string::npos || !type.ends_with('>'))
            {
                spelling.name = type;
                return spelling;
            }

            spelling.name = type.substr(0, open);

            const std::string inner = type.substr(open + 1, type.size() - open - 2);
            int depth = 0;
            std::string current;
            for (char c : inner)
            {
                if (c == '<')
                {
                    ++depth;
                }
                else if (c == '>')
                {
                    --depth;
                }
                else if (c == ',' && depth == 0)
                {
                    spelling.arguments.push_back(Trimmed(current));
                    current.clear();
                    continue;
                }
                current += c;
            }
            if (!current.empty())
            {
                spelling.arguments.push_back(Trimmed(current));
            }
            return spelling;
        }

        void EmitAtNode(TSNode node, DiagnosticContext &ctx, std::string_view code, std::string_view arg)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, arg);
        }

        /**
         * @brief The declared return type of the function a `return` sits in, or "" when unknown.
         */
        std::string EnclosingReturnType(TSNode returnNode, std::string_view sourceCode)
        {
            for (TSNode parent = ts_node_parent(returnNode); !ts_node_is_null(parent);
                 parent = ts_node_parent(parent))
            {
                const std::string_view type = NodeType(parent);
                if (type == "lambda_expression")
                {
                    return "";
                }
                if (type == "func_declaration")
                {
                    TSNode returnType = parser::GetChildByField(parent, parser::fields::ReturnType);
                    if (ts_node_is_null(returnType))
                    {
                        returnType = parser::GetChildByField(parent, parser::fields::Type);
                    }
                    return ts_node_is_null(returnType) ? std::string()
                                                       : GetNodeText(returnType, sourceCode);
                }
            }
            return "";
        }

        /**
         * @brief How many values a list writes, counting the ones that were left out.
         */
        uint32_t ListValueCount(TSNode listNode)
        {
            uint32_t separators = 0;
            uint32_t written = 0;
            const uint32_t count = ts_node_child_count(listNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(listNode, i);
                const std::string_view type = NodeType(child);
                if (type == ",")
                {
                    ++separators;
                }
                else if (ts_node_is_named(child) && type != "comment")
                {
                    ++written;
                }
            }

            if (separators > 0)
            {
                return separators + 1;
            }
            return written > 0 ? 1 : 0;
        }

        struct ElementContext
        {
            std::string_view sourceCode;
            const Scope *scopeRoot = nullptr;
        };

        void CheckElementValue(TSNode element,
                               const std::string &wanted,
                               DiagnosticContext &ctx,
                               const ElementContext &elements)
        {
            if (wanted.empty() || wanted == "?")
            {
                return;
            }

            const std::string actual = ResolveExpressionType(element, elements.scopeRoot,
                                                             ctx.request.symbolTable,
                                                             elements.sourceCode,
                                                             ctx.request.fileUri);
            if (actual.empty())
            {
                return;
            }

            if (!CanConvertImplicitly(actual, wanted, ctx))
            {
                const TSPoint start = ts_node_start_point(element);
                const TSPoint end = ts_node_end_point(element);
                ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                "as-err-no-implicit-conversion", StripDecorations(actual),
                                StripDecorations(wanted));
            }
        }

        struct ArrayTypeInfo
        {
            bool isArray = false;
            std::string elementType;
        };

        ArrayTypeInfo InspectArrayType(const std::string &type,
                                       const TemplateSpelling &spelling,
                                       const DiagnosticContext &ctx,
                                       const std::unordered_set<std::string> &arrayLikeTemplates)
        {
            if (type.ends_with("[]"))
            {
                return { true, type.substr(0, type.size() - 2) };
            }
            if (spelling.name == "array" ||
                spelling.name == ctx.request.GetArrayTypeName() ||
                arrayLikeTemplates.contains(spelling.name))
            {
                std::string elem = spelling.arguments.empty() ? "auto" : spelling.arguments[0];
                return { true, std::move(elem) };
            }
            return { false, "" };
        }

        void ValidateList(TSNode listNode,
                          const std::string &targetType,
                          DiagnosticContext &ctx,
                          const ElementContext &elements,
                          const std::unordered_set<std::string> &arrayLikeTemplates,
                          int depth)
        {
            if (depth >= k_maxAstDepth || ts_node_is_null(listNode))
            {
                return;
            }

            const std::string type = StripDecorations(targetType);
            if (type.empty() || type == "auto" || type == "void")
            {
                return;
            }

            if (IsCorePrimitive(type) || type == "?")
            {
                EmitAtNode(listNode, ctx, "as-err-initializer-list-not-supported", type);
                return;
            }

            const TemplateSpelling spelling = ReadTemplateSpelling(type);

            // 1. Array types
            const ArrayTypeInfo arrayInfo = InspectArrayType(type, spelling, ctx, arrayLikeTemplates);
            if (arrayInfo.isArray)
            {
                const uint32_t count = ts_node_named_child_count(listNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode child = ts_node_named_child(listNode, i);
                    if (NodeType(child) == "initializer_list")
                    {
                        ValidateList(child, arrayInfo.elementType, ctx, elements, arrayLikeTemplates, depth + 1);
                    }
                    else
                    {
                        CheckElementValue(child, arrayInfo.elementType, ctx, elements);
                    }
                }
                return;
            }

            // 2. Dictionary types
            if (spelling.name == "dictionary" || spelling.name == "dict")
            {
                const uint32_t count = ts_node_named_child_count(listNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode child = ts_node_named_child(listNode, i);
                    if (NodeType(child) != "initializer_list")
                    {
                        EmitAtNode(child, ctx, "as-err-initializer-list-expected", "");
                        continue;
                    }

                    const uint32_t valuesCount = ListValueCount(child);
                    if (valuesCount < 2)
                    {
                        EmitAtNode(child, ctx, "as-err-initializer-list-too-few", "");
                    }
                    else if (valuesCount > 2)
                    {
                        EmitAtNode(child, ctx, "as-err-initializer-list-too-many", "");
                    }

                    const uint32_t pairCount = ts_node_named_child_count(child);
                    if (pairCount > 0)
                    {
                        TSNode keyNode = ts_node_named_child(child, 0);
                        if (NodeType(keyNode) == "initializer_list")
                        {
                            EmitAtNode(keyNode, ctx, "as-err-initializer-list-not-supported", "string");
                        }
                        else
                        {
                            CheckElementValue(keyNode, "string", ctx, elements);
                        }
                    }
                    if (pairCount > 1)
                    {
                        TSNode valNode = ts_node_named_child(child, 1);
                        if (NodeType(valNode) == "initializer_list")
                        {
                            EmitAtNode(valNode, ctx, "as-err-initializer-list-not-supported", "?");
                        }
                        else
                        {
                            CheckElementValue(valNode, "?", ctx, elements);
                        }
                    }
                }
                return;
            }

            // 3. 2D grid
            if (spelling.name == "grid")
            {
                const std::string elemType = spelling.arguments.empty() ? "auto" : spelling.arguments[0];
                const uint32_t count = ts_node_named_child_count(listNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode child = ts_node_named_child(listNode, i);
                    if (NodeType(child) != "initializer_list")
                    {
                        EmitAtNode(child, ctx, "as-err-initializer-list-expected", "");
                    }
                    else
                    {
                        const uint32_t colCount = ts_node_named_child_count(child);
                        for (uint32_t j = 0; j < colCount; ++j)
                        {
                            TSNode cell = ts_node_named_child(child, j);
                            if (NodeType(cell) == "initializer_list")
                            {
                                ValidateList(cell, elemType, ctx, elements, arrayLikeTemplates, depth + 1);
                            }
                            else
                            {
                                CheckElementValue(cell, elemType, ctx, elements);
                            }
                        }
                    }
                }
                return;
            }

            // 4. Class / Struct Aggregates
            const auto symbols = ctx.request.symbolTable.FindSymbolsPtr(spelling.name);
            if (symbols)
            {
                for (const auto &sym : *symbols)
                {
                    if (sym.type == SymbolType::Class)
                    {
                        std::vector<std::pair<std::string, std::string>> fields;
                        const auto &members = ctx.request.GetRuleIndex().Members(spelling.name);
                        for (const auto &key : members.memberKeys)
                        {
                            if (const auto syms = ctx.request.symbolTable.FindSymbolsPtr(key))
                            {
                                for (const auto &s : *syms)
                                {
                                    if (s.type == SymbolType::Variable)
                                    {
                                        fields.push_back({ s.name, s.GetVariable().baseTypeName });
                                    }
                                }
                            }
                        }

                        if (fields.empty() && (spelling.name == "complex" || sym.name == "complex"))
                        {
                            fields = { { "r", "float" }, { "i", "float" } };
                        }

                        if (!fields.empty())
                        {
                            const uint32_t written = ListValueCount(listNode);
                            const auto expected = static_cast<uint32_t>(fields.size());
                            if (written < expected)
                            {
                                EmitAtNode(listNode, ctx, "as-err-initializer-list-too-few", "");
                                return;
                            }
                            if (written > expected)
                            {
                                EmitAtNode(listNode, ctx, "as-err-initializer-list-too-many", "");
                                return;
                            }

                            const uint32_t childCount = ts_node_named_child_count(listNode);
                            for (uint32_t i = 0; i < childCount && i < fields.size(); ++i)
                            {
                                TSNode elem = ts_node_named_child(listNode, i);
                                if (NodeType(elem) == "initializer_list")
                                {
                                    ValidateList(elem, fields[i].second, ctx, elements, arrayLikeTemplates, depth + 1);
                                }
                                else
                                {
                                    CheckElementValue(elem, fields[i].second, ctx, elements);
                                }
                            }
                            return;
                        }

                        // A class with no member fields or list pattern
                        if (depth == 0 && ctx.request.symbolTable.HasSymbolAnywhere(type) &&
                            !ctx.request.IsRegisteredSymbol(type))
                        {
                            const TSPoint start = ts_node_start_point(listNode);
                            const TSPoint end = ts_node_end_point(listNode);
                            ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                            "as-hint-list-pattern-unknown", type, DiagnosticSeverity::Hint);
                        }
                        return;
                    }
                }
            }
        }
    }

    void CheckInitializerListAgainstType(TSNode listNode,
                                         const std::string &targetType,
                                         std::string_view sourceCode,
                                         const Scope *scope,
                                         DiagnosticContext &ctx)
    {
        ValidateList(listNode, targetType, ctx, ElementContext{ sourceCode, scope },
                     ctx.request.GetArrayLikeTemplateNames(), 0);
    }

    void ValidateInitializerList(TSNode listNode, const std::string &expectedType, DiagnosticContext &ctx)
    {
        ElementContext elements{ ctx.request.sourceCode, ctx.request.scopeRoot.get() };
        ValidateList(listNode, expectedType, ctx, elements, ctx.request.GetArrayLikeTemplateNames(), 0);
    }

    void CheckInitializerLists(const InitializerListCheckRequest &request, DiagnosticContext &ctx)
    {
        const std::unordered_set<std::string> arrayLikeTemplates = ctx.request.GetArrayLikeTemplateNames();

        // The scope the list sits in, not the document's root. Resolving a name walks a scope chain
        // *upwards*, so a root handed to a list inside a function resolves globals and nothing else:
        // `array<int> a = {someLocal}` had no way to type its own element.
        const auto elementsAt = [&](TSNode node)
        {
            const TSPoint start = ts_node_start_point(node);
            return ElementContext{ request.sourceCode,
                                   request.scopeRoot
                                       ? FindEnclosingScope(request.scopeRoot, start.row, start.column)
                                       : nullptr };
        };

        std::vector<TSNode> stack = { request.root };
        while (!stack.empty())
        {
            TSNode node = stack.back();
            stack.pop_back();

            const std::string_view nodeType = NodeType(node);

            // Every position the grammar lets a list appear in, and every one of them compiles:
            // `take({1,2})`, `a = {1,2}` and `return {1,2};` are all accepted by the real compiler,
            // which infers the target type from the parameter, the assignee and the declared return
            // type in turn. Only the declaration was visited here, so the other three were checked
            // nowhere - and the argument case is judged from CallChecker, which is the pass that
            // knows which overload was picked.
            if (nodeType == "typed_initializer_list")
            {
                // `array<int> = {1, 2}` - AngelScript's anonymous object. The target type is
                // written at the list, so nothing has to be inferred to check it.
                TSNode typeNode = parser::GetChildByField(node, parser::fields::Type);
                TSNode valueNode = parser::GetChildByField(node, parser::fields::Value);
                if (!ts_node_is_null(typeNode) && !ts_node_is_null(valueNode))
                {
                    ValidateList(valueNode, GetNodeText(typeNode, request.sourceCode), ctx,
                                 elementsAt(valueNode), arrayLikeTemplates, 0);
                }
            }
            else if (nodeType == "assignment_expression")
            {
                TSNode value = parser::GetChildByField(node, parser::fields::Right);
                if (!ts_node_is_null(value) && NodeType(value) == "initializer_list")
                {
                    // Plain `=` only. A compound assignment takes no list at all - the compiler
                    // answers `a += {1};` with "Illegal operation on 'int[]&'" - and that is a
                    // verdict about the operator, not about the list, so it is left to say
                    // nothing rather than blamed on the shape.
                    TSNode opNode = parser::GetChildByField(node, parser::fields::Operator);
                    TSNode target = parser::GetChildByField(node, parser::fields::Left);
                    if (!ts_node_is_null(opNode) && !ts_node_is_null(target) &&
                        GetNodeText(opNode, request.sourceCode) == "=")
                    {
                        const ElementContext elements = elementsAt(value);
                        const std::string targetType = ResolveExpressionType(
                            target, elements.scopeRoot, ctx.request.symbolTable,
                            request.sourceCode, ctx.request.fileUri);
                        if (!targetType.empty())
                        {
                            ValidateList(value, targetType, ctx, elements, arrayLikeTemplates, 0);
                        }
                    }
                }
            }
            else if (nodeType == "return_statement")
            {
                if (ts_node_named_child_count(node) > 0)
                {
                    TSNode value = ts_node_named_child(node, 0);
                    if (NodeType(value) == "initializer_list")
                    {
                        // A lambda stops the walk: `function() { return {1}; }` returns into a
                        // funcdef this pass never sees, and guessing which one is not a verdict.
                        const std::string returnType = EnclosingReturnType(node, request.sourceCode);
                        if (!returnType.empty())
                        {
                            ValidateList(value, returnType, ctx, elementsAt(value),
                                         arrayLikeTemplates, 0);
                        }
                    }
                }
            }
            else if (nodeType == "variable_declaration")
            {
                TSNode typeNode = parser::GetChildByField(node, parser::fields::VarType);
                if (ts_node_is_null(typeNode))
                {
                    typeNode = parser::GetChildByField(node, parser::fields::Type);
                }

                if (!ts_node_is_null(typeNode))
                {
                    // The declared type is read from the source rather than resolved, because the
                    // only thing needed from it is its spelling - the name and the arguments - and
                    // that is written down verbatim. Resolution would add a way to be wrong without
                    // adding anything to be right.
                    const std::string declaredType = GetNodeText(typeNode, request.sourceCode);

                    const uint32_t childCount = ts_node_named_child_count(node);
                    for (uint32_t i = 0; i < childCount; ++i)
                    {
                        TSNode declarator = ts_node_named_child(node, i);
                        if (NodeType(declarator) != "variable_declarator")
                        {
                            continue;
                        }

                        TSNode valueNode = parser::GetChildByField(declarator, parser::fields::Value);
                        if (!ts_node_is_null(valueNode) && NodeType(valueNode) == "initializer_list")
                        {
                            ValidateList(valueNode, declaredType, ctx, elementsAt(valueNode),
                                         arrayLikeTemplates, 0);
                        }
                    }
                }
            }

            const uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                stack.push_back(ts_node_child(node, i));
            }
        }
    }
}
