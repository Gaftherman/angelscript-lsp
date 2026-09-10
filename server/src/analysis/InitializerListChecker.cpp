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
#include <ankerl/unordered_dense.h>
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

        /**
         * @brief Container structure and dimensionality information.
         */
        struct ContainerInfo
        {
            bool isContainer = false;
            uint32_t dimensions = 1;
            std::string elementType;
        };

        /**
         * @brief Checks whether a constructor function matches AngelScript's list behavior signature.
         *
         * Conforming to `asBEHAVE_LIST_CONSTRUCT` / `asBEHAVE_LIST_FACTORY`:
         * - Single parameter: integer reference with in modifier (e.g. `const int &in` or `int &in`).
         * - Two parameters: both integer references with in modifier (e.g. `int &in, int &in`).
         */
        bool IsListConstructorSignature(const FunctionSignature &fn)
        {
            if (fn.parameters.size() == 1)
            {
                return fn.parameters[0].modifier == ParameterModifier::In &&
                       (fn.parameters[0].typeName.find("int") != std::string::npos ||
                        fn.parameters[0].baseTypeName == "int" ||
                        fn.parameters[0].baseTypeName == "uint");
            }
            if (fn.parameters.size() == 2)
            {
                return fn.parameters[0].modifier == ParameterModifier::In &&
                       fn.parameters[1].modifier == ParameterModifier::In &&
                       (fn.parameters[0].typeName.find("int") != std::string::npos ||
                        fn.parameters[0].baseTypeName == "int" ||
                        fn.parameters[0].baseTypeName == "uint") &&
                       (fn.parameters[1].typeName.find("int") != std::string::npos ||
                        fn.parameters[1].baseTypeName == "int" ||
                        fn.parameters[1].baseTypeName == "uint");
            }
            return false;
        }

        /**
         * @brief Inspects whether a type represents a sequence or multi-dimensional container.
         *
         * Identifies containers via:
         * 1. Syntax arrays: `T[]`.
         * 2. Engine-configured array type or `arrayLikeTemplates`.
         * 3. SymbolTable registration with multi-index `opIndex` (e.g. 2D grid/matrix) or list constructors.
         */
        ContainerInfo InspectContainer(const std::string &type,
                                       const TemplateSpelling &spelling,
                                       const DiagnosticContext &ctx,
                                       const std::unordered_set<std::string> &arrayLikeTemplates)
        {
            if (type.ends_with("[]"))
            {
                return { true, 1, type.substr(0, type.size() - 2) };
            }

            const std::string_view configuredArray = ctx.request.GetArrayTypeName();
            if (spelling.name == "array" ||
                (!configuredArray.empty() && spelling.name == configuredArray) ||
                arrayLikeTemplates.contains(spelling.name))
            {
                std::string elem = spelling.arguments.empty() ? "auto" : spelling.arguments[0];
                return { true, 1, std::move(elem) };
            }

            if (!spelling.name.empty())
            {
                std::vector<Symbol> typeSymbols;
                if (const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name))
                {
                    typeSymbols = *syms;
                }
                else
                {
                    typeSymbols = ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);
                }

                for (const auto &sym : typeSymbols)
                {
                    if (sym.type != SymbolType::Class)
                    {
                        continue;
                    }

                    uint32_t maxIndexParams = 0;
                    std::string indexReturnType;
                    bool hasListConstructor = false;

                    std::vector<std::string> containers;
                    if (!sym.qualifiedName.empty())
                    {
                        containers.push_back(sym.qualifiedName);
                    }
                    if (!sym.name.empty() && sym.name != sym.qualifiedName)
                    {
                        containers.push_back(sym.name);
                    }
                    if (!spelling.name.empty() && spelling.name != sym.name && spelling.name != sym.qualifiedName)
                    {
                        containers.push_back(spelling.name);
                    }

                    for (const auto &containerKey : containers)
                    {
                        const auto &members = ctx.request.GetRuleIndex().Members(containerKey);
                        for (const auto &key : members.memberKeys)
                        {
                            if (const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key))
                            {
                                for (const auto &m : *mSyms)
                                {
                                    if (m.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(m.signature))
                                    {
                                        const auto &fn = m.GetFunction();
                                        if (m.name == "opIndex")
                                        {
                                            if (fn.parameters.size() > maxIndexParams)
                                            {
                                                maxIndexParams = static_cast<uint32_t>(fn.parameters.size());
                                                indexReturnType = fn.returnBaseTypeName.empty()
                                                    ? fn.returnType
                                                    : fn.returnBaseTypeName;
                                            }
                                        }
                                        else if (m.name == sym.name)
                                        {
                                            if (IsListConstructorSignature(fn))
                                            {
                                                hasListConstructor = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (maxIndexParams >= 2)
                    {
                        std::string elem = !spelling.arguments.empty()
                            ? spelling.arguments[0]
                            : StripDecorations(indexReturnType);
                        if (elem.empty())
                        {
                            elem = "auto";
                        }
                        return { true, maxIndexParams, std::move(elem) };
                    }

                    if (hasListConstructor && (!spelling.arguments.empty() || maxIndexParams == 1))
                    {
                        std::string elem = !spelling.arguments.empty()
                            ? spelling.arguments[0]
                            : StripDecorations(indexReturnType);
                        if (elem.empty())
                        {
                            elem = "auto";
                        }
                        return { true, 1, std::move(elem) };
                    }
                }
            }

            return { false, 1, "" };
        }

        /**
         * @brief Dictionary container key and value type information.
         */
        struct DictInfo
        {
            bool isDict = false;
            std::string keyType;
            std::string valType;
        };

        /**
         * @brief Inspects whether a type represents an associative container and resolves its key and value types.
         */
        DictInfo InspectDictionary(const TemplateSpelling &spelling,
                                   const DiagnosticContext &ctx)
        {
            if (spelling.name.empty())
            {
                return { false, "", "" };
            }

            std::string keyType;
            std::string valType;
            bool isAssociative = false;

            if (spelling.arguments.size() >= 2)
            {
                keyType = spelling.arguments[0];
                valType = spelling.arguments[1];
            }
            else if (spelling.arguments.size() == 1)
            {
                valType = spelling.arguments[0];
            }

            std::vector<Symbol> typeSymbols;
            if (const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name))
            {
                typeSymbols = *syms;
            }
            else
            {
                typeSymbols = ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);
            }

            for (const auto &sym : typeSymbols)
            {
                if (sym.type != SymbolType::Class)
                {
                    continue;
                }

                std::vector<std::string> containers;
                if (!sym.qualifiedName.empty())
                {
                    containers.push_back(sym.qualifiedName);
                }
                if (!sym.name.empty() && sym.name != sym.qualifiedName)
                {
                    containers.push_back(sym.name);
                }
                if (!spelling.name.empty() && spelling.name != sym.name && spelling.name != sym.qualifiedName)
                {
                    containers.push_back(spelling.name);
                }

                for (const auto &containerKey : containers)
                {
                    const auto &members = ctx.request.GetRuleIndex().Members(containerKey);
                    if (members.methodNames.contains("exists"))
                    {
                        isAssociative = true;
                    }

                    if (spelling.arguments.size() >= 2 &&
                        (members.methodNames.contains("insert") || members.methodNames.contains("opIndex") ||
                         members.methodNames.contains("find") || members.methodNames.contains("set")))
                    {
                        isAssociative = true;
                    }

                    for (const auto &key : members.memberKeys)
                    {
                        if (const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key))
                        {
                            for (const auto &m : *mSyms)
                            {
                                if (m.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(m.signature))
                                {
                                    const auto &fn = m.GetFunction();
                                    if (m.name == "set" && fn.parameters.size() >= 2)
                                    {
                                        isAssociative = true;
                                        if (keyType.empty())
                                        {
                                            keyType = fn.parameters[0].baseTypeName.empty()
                                                ? fn.parameters[0].typeName
                                                : fn.parameters[0].baseTypeName;
                                        }
                                        if (valType.empty())
                                        {
                                            valType = fn.parameters[1].baseTypeName.empty()
                                                ? fn.parameters[1].typeName
                                                : fn.parameters[1].baseTypeName;
                                        }
                                    }
                                    else if (m.name == "exists" && !fn.parameters.empty())
                                    {
                                        isAssociative = true;
                                        if (keyType.empty())
                                        {
                                            keyType = fn.parameters[0].baseTypeName.empty()
                                                ? fn.parameters[0].typeName
                                                : fn.parameters[0].baseTypeName;
                                        }
                                    }
                                    else if (m.name == "get" && fn.parameters.size() >= 2)
                                    {
                                        isAssociative = true;
                                        if (keyType.empty())
                                        {
                                            keyType = fn.parameters[0].baseTypeName.empty()
                                                ? fn.parameters[0].typeName
                                                : fn.parameters[0].baseTypeName;
                                        }
                                        if (valType.empty())
                                        {
                                            valType = fn.parameters[1].baseTypeName.empty()
                                                ? fn.parameters[1].typeName
                                                : fn.parameters[1].baseTypeName;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!isAssociative)
            {
                return { false, "", "" };
            }

            if (keyType.empty())
            {
                keyType = "string";
            }
            if (valType.empty())
            {
                valType = "?";
            }

            return { true, StripDecorations(keyType), StripDecorations(valType) };
        }

        /**
         * @brief Field information for aggregate struct member properties.
         */
        struct StructFieldInfo
        {
            std::string name;
            std::string type;
            uint32_t line = 0;
            uint32_t col = 0;
        };

        /**
         * @brief Aggregate struct fields and list initialization capability.
         */
        struct AggregateStructInfo
        {
            bool hasListSupport = false;
            std::vector<StructFieldInfo> fields;
        };

        using StructLayoutCache = ankerl::unordered_dense::map<std::string, AggregateStructInfo>;

        /**
         * @brief Dynamically extracts member properties in declaration order for an aggregate struct/class.
         */
        AggregateStructInfo InspectAggregateStruct(const TemplateSpelling &spelling,
                                                   const DiagnosticContext &ctx,
                                                   StructLayoutCache &cache)
        {
            if (spelling.name.empty())
            {
                return {};
            }

            const auto it = cache.find(spelling.name);
            if (it != cache.end())
            {
                return it->second;
            }

            AggregateStructInfo info;

            if (ctx.request.IsRegisteredSymbol(spelling.name))
            {
                info.hasListSupport = true;
            }

            std::vector<Symbol> typeSymbols;
            if (const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name))
            {
                typeSymbols = *syms;
            }
            else
            {
                typeSymbols = ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);
            }

            for (const auto &sym : typeSymbols)
            {
                if (sym.type != SymbolType::Class)
                {
                    continue;
                }

                if (ctx.request.IsRegisteredSymbol(sym.name) ||
                    (!sym.qualifiedName.empty() && ctx.request.IsRegisteredSymbol(sym.qualifiedName)))
                {
                    info.hasListSupport = true;
                }

                if (IsFromPredefinedStub(sym, ctx))
                {
                    info.hasListSupport = true;
                }

                std::vector<std::string> candidateContainers;
                if (!sym.qualifiedName.empty())
                {
                    candidateContainers.push_back(sym.qualifiedName);
                }
                if (!sym.name.empty() && sym.name != sym.qualifiedName)
                {
                    candidateContainers.push_back(sym.name);
                }
                if (!spelling.name.empty() && spelling.name != sym.name && spelling.name != sym.qualifiedName)
                {
                    candidateContainers.push_back(spelling.name);
                }

                for (const auto &container : candidateContainers)
                {
                    const auto &members = ctx.request.GetRuleIndex().Members(container);
                    for (const auto &key : members.memberKeys)
                    {
                        if (const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key))
                        {
                            for (const auto &s : *mSyms)
                            {
                                if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
                                {
                                    if (std::holds_alternative<VariableSignature>(s.signature))
                                    {
                                        const auto &varSig = s.GetVariable();
                                        if (!varSig.isVirtualProperty)
                                        {
                                            std::string fType = varSig.baseTypeName.empty()
                                                ? varSig.typeName
                                                : varSig.baseTypeName;
                                            info.fields.push_back({ s.name, StripDecorations(fType), s.startLine, s.startCharacter });
                                        }
                                    }
                                }
                                else if (s.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(s.signature))
                                {
                                    if (s.name == sym.name)
                                    {
                                        const auto &fn = s.GetFunction();
                                        if (IsListConstructorSignature(fn))
                                        {
                                            info.hasListSupport = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            std::sort(info.fields.begin(), info.fields.end(),
                      [](const StructFieldInfo &a, const StructFieldInfo &b)
                      {
                          if (a.line != b.line)
                          {
                              return a.line < b.line;
                          }
                          if (a.col != b.col)
                          {
                              return a.col < b.col;
                          }
                          return a.name < b.name;
                      });

            info.fields.erase(std::unique(info.fields.begin(), info.fields.end(),
                                          [](const StructFieldInfo &a, const StructFieldInfo &b)
                                          {
                                              return a.name == b.name && a.line == b.line && a.col == b.col;
                                          }),
                              info.fields.end());

            cache.emplace(spelling.name, info);
            return info;
        }

        void ValidateList(TSNode listNode,
                          const std::string &targetType,
                          DiagnosticContext &ctx,
                          const ElementContext &elements,
                          const std::unordered_set<std::string> &arrayLikeTemplates,
                          int depth,
                          StructLayoutCache &cache);

        /**
         * @brief Validates elements of a sequence or multi-dimensional container recursively.
         */
        void ValidateMultiDimensionalContainer(TSNode node,
                                               uint32_t dimension,
                                               const std::string &elemType,
                                               DiagnosticContext &ctx,
                                               const ElementContext &elements,
                                               const std::unordered_set<std::string> &arrayLikeTemplates,
                                               int depth,
                                               StructLayoutCache &cache)
        {
            if (depth >= k_maxAstDepth || ts_node_is_null(node))
            {
                return;
            }

            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_named_child(node, i);
                if (dimension > 1)
                {
                    if (NodeType(child) != "initializer_list")
                    {
                        EmitAtNode(child, ctx, "as-err-initializer-list-expected", "");
                    }
                    else
                    {
                        ValidateMultiDimensionalContainer(child, dimension - 1, elemType,
                                                          ctx, elements, arrayLikeTemplates, depth + 1, cache);
                    }
                }
                else
                {
                    if (NodeType(child) == "initializer_list")
                    {
                        ValidateList(child, elemType, ctx, elements, arrayLikeTemplates, depth + 1, cache);
                    }
                    else
                    {
                        CheckElementValue(child, elemType, ctx, elements);
                    }
                }
            }
        }

        void ValidateList(TSNode listNode,
                          const std::string &targetType,
                          DiagnosticContext &ctx,
                          const ElementContext &elements,
                          const std::unordered_set<std::string> &arrayLikeTemplates,
                          int depth,
                          StructLayoutCache &cache)
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

            // 1. Dictionary types
            const DictInfo dictInfo = InspectDictionary(spelling, ctx);
            if (dictInfo.isDict)
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
                            EmitAtNode(keyNode, ctx, "as-err-initializer-list-not-supported", dictInfo.keyType);
                        }
                        else
                        {
                            CheckElementValue(keyNode, dictInfo.keyType, ctx, elements);
                        }
                    }
                    if (pairCount > 1)
                    {
                        TSNode valNode = ts_node_named_child(child, 1);
                        if (NodeType(valNode) == "initializer_list")
                        {
                            EmitAtNode(valNode, ctx, "as-err-initializer-list-not-supported", dictInfo.valType);
                        }
                        else
                        {
                            CheckElementValue(valNode, dictInfo.valType, ctx, elements);
                        }
                    }
                }
                return;
            }

            // 2. Sequence & multi-dimensional container types
            const ContainerInfo containerInfo = InspectContainer(type, spelling, ctx, arrayLikeTemplates);
            if (containerInfo.isContainer)
            {
                ValidateMultiDimensionalContainer(listNode, containerInfo.dimensions, containerInfo.elementType,
                                                  ctx, elements, arrayLikeTemplates, depth, cache);
                return;
            }

            // 3. Class / Struct Aggregates
            const AggregateStructInfo structInfo = InspectAggregateStruct(spelling, ctx, cache);
            if (!structInfo.fields.empty())
            {
                if (!structInfo.hasListSupport)
                {
                    EmitAtNode(listNode, ctx, "as-err-initializer-list-not-supported", type);
                    return;
                }

                const uint32_t written = ListValueCount(listNode);
                const auto expected = static_cast<uint32_t>(structInfo.fields.size());
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
                for (uint32_t i = 0; i < childCount && i < structInfo.fields.size(); ++i)
                {
                    TSNode elem = ts_node_named_child(listNode, i);
                    if (NodeType(elem) == "initializer_list")
                    {
                        ValidateList(elem, structInfo.fields[i].type, ctx, elements, arrayLikeTemplates, depth + 1, cache);
                    }
                    else
                    {
                        CheckElementValue(elem, structInfo.fields[i].type, ctx, elements);
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
        }
    }

    void CheckInitializerListAgainstType(TSNode listNode,
                                         const std::string &targetType,
                                         std::string_view sourceCode,
                                         const Scope *scope,
                                         DiagnosticContext &ctx)
    {
        StructLayoutCache cache;
        ValidateList(listNode, targetType, ctx, ElementContext{ sourceCode, scope },
                     ctx.request.GetArrayLikeTemplateNames(), 0, cache);
    }

    void ValidateInitializerList(TSNode listNode, const std::string &expectedType, DiagnosticContext &ctx)
    {
        ElementContext elements{ ctx.request.sourceCode, ctx.request.scopeRoot.get() };
        StructLayoutCache cache;
        ValidateList(listNode, expectedType, ctx, elements, ctx.request.GetArrayLikeTemplateNames(), 0, cache);
    }


    void CheckInitializerLists(const InitializerListCheckRequest &request, DiagnosticContext &ctx)
    {
        const std::unordered_set<std::string> arrayLikeTemplates = ctx.request.GetArrayLikeTemplateNames();
        StructLayoutCache structCache;

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
                                 elementsAt(valueNode), arrayLikeTemplates, 0, structCache);
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
                            ValidateList(value, targetType, ctx, elements, arrayLikeTemplates, 0, structCache);
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
                                         arrayLikeTemplates, 0, structCache);
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
                                         arrayLikeTemplates, 0, structCache);
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
