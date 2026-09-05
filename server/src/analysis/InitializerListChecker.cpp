#include "analysis/InitializerListChecker.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/ListPattern.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

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

        /** @brief A type's list pattern together with the arguments to substitute into it. */
        struct ResolvedPattern
        {
            ListPattern pattern;
            std::vector<std::string> templateParameters;
            std::vector<std::string> templateArguments;
        };

        /**
         * @brief The pattern a written type accepts, or an invalid one when nothing says.
         *
         * Three sources, in order of how much they know:
         *
         *  1. `T[]` is answered from the grammar. The language spells an array that way whatever
         *     type the engine registered as its default array, so the suffix settles it.
         *  2. A `@listpattern` tag on the class declaration - the pattern as the host's own
         *     `asBEHAVE_LIST_FACTORY` registration spells it. This is the general mechanism, and it
         *     is the only thing that separates `array<T>` from `optional<T>`: they are declared
         *     identically and only one of them accepts a list at all.
         *  3. `--array-like-type`, shorthand for `{repeat T}`, for a host that would rather not
         *     edit its stub.
         *
         * Nothing else infers a pattern, and a type with no pattern leaves the caller silent.
         */
        ResolvedPattern ResolvePattern(const std::string &type,
                                       const DiagnosticContext &ctx,
                                       const std::unordered_set<std::string> &arrayLikeTemplates)
        {
            ResolvedPattern resolved;

            if (type.ends_with("[]"))
            {
                resolved.pattern = ParseListPattern("{repeat T}");
                resolved.templateParameters = { "T" };
                resolved.templateArguments = { type.substr(0, type.size() - 2) };
                return resolved;
            }

            const TemplateSpelling spelling = ReadTemplateSpelling(type);

            if (const auto symbols = ctx.request.symbolTable.FindSymbolsPtr(spelling.name))
            {
                for (const auto &symbol : *symbols)
                {
                    if (symbol.type != SymbolType::Class ||
                        !std::holds_alternative<ClassSignature>(symbol.signature))
                    {
                        continue;
                    }

                    const auto &cls = std::get<ClassSignature>(symbol.signature);
                    if (cls.listPattern.empty())
                    {
                        continue;
                    }

                    resolved.pattern = ParseListPattern(cls.listPattern);
                    resolved.templateParameters = cls.templateParams;
                    resolved.templateArguments = spelling.arguments;
                    return resolved;
                }
            }

            if (arrayLikeTemplates.contains(spelling.name) && spelling.arguments.size() == 1)
            {
                resolved.pattern = ParseListPattern("{repeat T}");
                resolved.templateParameters = { "T" };
                resolved.templateArguments = spelling.arguments;
            }
            return resolved;
        }

        /** @brief Replaces a pattern's template parameter with the argument written at the use site. */
        std::string SubstituteParameter(const std::string &patternType, const ResolvedPattern &resolved)
        {
            for (size_t i = 0; i < resolved.templateParameters.size(); ++i)
            {
                if (patternType == resolved.templateParameters[i] && i < resolved.templateArguments.size())
                {
                    return resolved.templateArguments[i];
                }
            }
            return patternType;
        }

        void EmitAtNode(TSNode node, DiagnosticContext &ctx, std::string_view code, std::string_view arg)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, arg);
        }

        /**
         * @brief The declared return type of the function a `return` sits in, or "" when unknown.
         *
         * Walks outward and stops at a lambda: `function() { return {1}; }` returns into whichever
         * funcdef the lambda is being assigned to, which is not written anywhere near the list and
         * is not a guess this pass makes. `void` comes back as itself and is passed over further
         * down, where every type that accepts no list is.
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
                    TSNode returnType = ts_node_child_by_field_name(parent, "return_type", 11);
                    if (ts_node_is_null(returnType))
                    {
                        returnType = ts_node_child_by_field_name(parent, "type", k_typeFieldLength);
                    }
                    return ts_node_is_null(returnType) ? std::string()
                                                       : GetNodeText(returnType, sourceCode);
                }
            }
            return "";
        }

        /**
         * @brief How many values a list writes, counting the ones that were left out.
         *
         * Counted from the separators rather than from the nodes, because an omitted element
         * produces no node at all: `{ 0, 1, , 4, 5 }` is five values to the compiler and four
         * children to the grammar (tests/parity/doc_g03_omitted_initlist_element.as). A hole is a
         * value - it takes the type's default - and the compiler counts it as one, which
         * `dictionary d = {{'a',}};` proves by compiling: the pattern's second slot is filled by
         * the hole. Counting children would have made that a "Not enough values" report on legal
         * code, which is the one failure mode this project does not accept.
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

        /**
         * @brief What typing an element's expression needs: the document text and its scope tree.
         *
         * Carried as one struct rather than two more parameters because it threads through every
         * function in this file and neither half is useful without the other. A null scopeRoot is
         * legal - a literal still types, a name does not, and an untyped element is passed over.
         */
        struct ElementContext
        {
            std::string_view sourceCode;
            const Scope *scopeRoot = nullptr;
        };

        /**
         * @brief Reports an element whose value cannot reach the type the pattern wants there.
         *
         * The list's shape is this pass's question; an element's type is the conversion pass's, and
         * that pass never sees one - it skips initializer lists outright. So the judgement is
         * borrowed through CanConvertImplicitly rather than reimplemented, which also borrows its
         * silence: an element or a target that does not resolve comes back convertible and nothing
         * is said.
         */
        void CheckElementValue(TSNode element,
                               const std::string &wanted,
                               DiagnosticContext &ctx,
                               const ElementContext &elements)
        {
            // `?` takes a value of any type at all, which is the whole point of it.
            if (wanted.empty() || ListPattern::IsAnyType(wanted))
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

        void ValidateList(TSNode listNode,
                          const std::string &targetType,
                          DiagnosticContext &ctx,
                          const ElementContext &elements,
                          const std::unordered_set<std::string> &arrayLikeTemplates,
                          int depth);

        struct ResolvedPattern;

        void ValidateSequence(TSNode listNode,
                              const ListPatternNode &group,
                              const ResolvedPattern &resolved,
                              DiagnosticContext &ctx,
                              const ElementContext &elements,
                              const std::unordered_set<std::string> &arrayLikeTemplates,
                              int depth);

        /**
         * @brief Checks one element of a list against the pattern item it has to match.
         *
         * The two directions are separate errors and the compiler words them differently: a list
         * where a value was wanted is "Initialization lists cannot be used with 'int'", and a value
         * where a list was wanted is "Expected a list enclosed by { } to match pattern" - which is
         * what `dictionary d = {1, 2};` gets, its pattern being `{repeat {string, ?}}`.
         */
        void ValidateElement(TSNode element,
                             const ListPatternNode &expected,
                             const ResolvedPattern &resolved,
                             DiagnosticContext &ctx,
                             const ElementContext &elements,
                             const std::unordered_set<std::string> &arrayLikeTemplates,
                             int depth)
        {
            const bool elementIsList = NodeType(element) == "initializer_list";

            if (expected.kind == ListPatternNode::Kind::Group)
            {
                if (!elementIsList)
                {
                    EmitAtNode(element, ctx, "as-err-initializer-list-expected", "");
                    return;
                }

                ValidateSequence(element, expected, resolved, ctx, elements, arrayLikeTemplates, depth + 1);
                return;
            }

            if (expected.kind != ListPatternNode::Kind::Type)
            {
                return;
            }

            const std::string wanted = SubstituteParameter(expected.typeName, resolved);

            if (!elementIsList)
            {
                // A plain expression where a value was wanted. The conversion pass does not reach
                // here - it skips initializer lists outright - so this was silent, and
                // `array<int> a = {"x"}` drew nothing where the compiler answers
                //
                //     ERROR (1, 32): Can't implicitly convert from 'const string' to 'int&'.
                //
                // The shape of the list is this pass's question and the type of an element is the
                // conversion pass's, so the judgement is borrowed rather than reimplemented, which
                // also borrows its silence: an element or a target that does not resolve comes back
                // convertible, and nothing is said.
                CheckElementValue(element, wanted, ctx, elements);
                return;
            }

            // `?` is AngelScript's variable type: it takes a *value* of any type, which is not the
            // same as taking anything at all. The compiler rejects `dictionary d = {{'a', {1}}};`
            // with "Initialization lists cannot be used with '?'", so it is reported like a
            // primitive - and for the same reason, that nothing can ever register a list factory
            // for it.
            if (ListPattern::IsAnyType(wanted))
            {
                EmitAtNode(element, ctx, "as-err-initializer-list-not-supported", wanted);
                return;
            }

            // A nested list is legitimate when the element type accepts one in turn - that is what
            // makes `array<array<int>> g = {{1,2},{3,4}}` correct and `array<int> a = {1,{2}}` not.
            ValidateList(element, wanted, ctx, elements, arrayLikeTemplates, depth + 1);
        }

        /**
         * @brief Walks one initializer list against the type it initializes.
         *
         * Reports a bare mismatch only when the target is a primitive, deliberately. Those are the
         * one family a host can never register a list factory for, so their silence in a stub
         * proves something; for any other type an absent pattern only means the stub did not say.
         */
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
            if (type.empty())
            {
                return;
            }

            if (IsCorePrimitive(type) && type != "void" && type != "auto")
            {
                EmitAtNode(listNode, ctx, "as-err-initializer-list-not-supported", type);
                return;
            }

            const ResolvedPattern resolved = ResolvePattern(type, ctx, arrayLikeTemplates);
            if (!resolved.pattern.valid)
            {
                // Nothing said what this type's list looks like, so nothing about the list can be
                // checked - not its shape, not its element types. Silence is right as a *verdict*
                // and useless as an explanation: the user sees a list going unchecked and has no
                // way to know that one doc tag would fix it.
                //
                // So a hint, at the declaration this is initialising, and only where a hint can be
                // acted on: the type has to be one the analyzer can actually see, because for an
                // engine-registered type there is no declaration to tag. That is the same
                // visibility test the rest of this pass makes, used here to decide whether to
                // suggest rather than whether to report.
                //
                // A Hint, not a warning. The code compiles - a list factory registered in C++ is
                // invisible to any stub - so this says the analyzer is missing something, never
                // that the script is.
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

            ValidateSequence(listNode, resolved.pattern.root, resolved, ctx, elements, arrayLikeTemplates, depth);
        }

        /**
         * @brief Matches a list's elements against the items of one pattern group.
         *
         * A `repeat` consumes every element from its position onward; the items before it match one
         * element each. That is the whole of the sequencing rule, and it applies at every depth -
         * `grid<T>`'s `{repeat {repeat_same T}}` nests one repeat inside another, so a group that
         * walked its children one-for-one would check the first cell of each row and no more.
         */
        void ValidateSequence(TSNode listNode,
                              const ListPatternNode &group,
                              const ResolvedPattern &resolved,
                              DiagnosticContext &ctx,
                              const ElementContext &elements,
                              const std::unordered_set<std::string> &arrayLikeTemplates,
                              int depth)
        {
            if (depth >= k_maxAstDepth)
            {
                return;
            }

            // A group with no `repeat` in it wants exactly as many values as it has items, and the
            // compiler says so in both directions - from tests/parity/doc_r18 and doc_r19, against
            // `dictionary`'s `{repeat {string, ?}}`:
            //
            //     dictionary d = {{'a'}};       Not enough values to match pattern
            //     dictionary d = {{'a', 1, 2}}; Too many values to match pattern
            //
            // Only for a fixed group. A `repeat` consumes every element from its position onward
            // and is satisfied by none at all - `array<int> a = {};` compiles - so a group holding
            // one has no count to check, which is every top-level list the two standard add-ons
            // accept.
            const bool hasRepeat = std::any_of(group.children.begin(), group.children.end(),
                                               [](const ListPatternNode &item)
                                               { return item.kind == ListPatternNode::Kind::Repeat; });
            if (!hasRepeat)
            {
                const uint32_t written = ListValueCount(listNode);
                const auto wanted = static_cast<uint32_t>(group.children.size());
                if (written < wanted)
                {
                    EmitAtNode(listNode, ctx, "as-err-initializer-list-too-few", "");
                    return;
                }
                if (written > wanted)
                {
                    EmitAtNode(listNode, ctx, "as-err-initializer-list-too-many", "");
                    return;
                }
            }

            const uint32_t count = ts_node_named_child_count(listNode);

            size_t patternIndex = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (patternIndex >= group.children.size())
                {
                    break;
                }

                const ListPatternNode &item = group.children[patternIndex];
                if (item.kind == ListPatternNode::Kind::Repeat)
                {
                    if (!item.children.empty())
                    {
                        ValidateElement(ts_node_named_child(listNode, i), item.children.front(),
                                        resolved, ctx, elements, arrayLikeTemplates, depth);
                    }
                    continue;   // Stays on the repeat for every remaining element.
                }

                ValidateElement(ts_node_named_child(listNode, i), item,
                                resolved, ctx, elements, arrayLikeTemplates, depth);
                ++patternIndex;
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
                TSNode typeNode = ts_node_child_by_field_name(node, "type", k_typeFieldLength);
                TSNode valueNode = ts_node_child_by_field_name(node, "value", k_valueFieldLength);
                if (!ts_node_is_null(typeNode) && !ts_node_is_null(valueNode))
                {
                    ValidateList(valueNode, GetNodeText(typeNode, request.sourceCode), ctx,
                                 elementsAt(valueNode), arrayLikeTemplates, 0);
                }
            }
            else if (nodeType == "assignment_expression")
            {
                TSNode value = ts_node_child_by_field_name(node, "right", 5);
                if (!ts_node_is_null(value) && NodeType(value) == "initializer_list")
                {
                    // Plain `=` only. A compound assignment takes no list at all - the compiler
                    // answers `a += {1};` with "Illegal operation on 'int[]&'" - and that is a
                    // verdict about the operator, not about the list, so it is left to say
                    // nothing rather than blamed on the shape.
                    TSNode opNode = ts_node_child_by_field_name(node, "operator", 8);
                    TSNode target = ts_node_child_by_field_name(node, "left", 4);
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
                TSNode typeNode = ts_node_child_by_field_name(node, "var_type", k_varTypeFieldLength);
                if (ts_node_is_null(typeNode))
                {
                    typeNode = ts_node_child_by_field_name(node, "type", k_typeFieldLength);
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

                        TSNode valueNode = ts_node_child_by_field_name(declarator, "value", k_valueFieldLength);
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
