#include "analysis/InitializerListChecker.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/ListPattern.h"
#include "analysis/SemanticHelpers.h"

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

    void CheckInitializerLists(const InitializerListCheckRequest &request, DiagnosticContext &ctx)
    {
        const std::unordered_set<std::string> arrayLikeTemplates = ctx.request.GetArrayLikeTemplateNames();
        const ElementContext elements{ request.sourceCode, request.scopeRoot };

        std::vector<TSNode> stack = { request.root };
        while (!stack.empty())
        {
            TSNode node = stack.back();
            stack.pop_back();

            if (NodeType(node) == "variable_declaration")
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
                            ValidateList(valueNode, declaredType, ctx, elements, arrayLikeTemplates, 0);
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
