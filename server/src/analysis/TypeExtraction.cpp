#include "analysis/TypeExtraction.h"
#include <tree_sitter/api.h>
#include <ankerl/unordered_dense.h>
#include <string_view>

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::analysis
{
    #define SYM_NAME(str) str, static_cast<uint32_t>(sizeof(str) - 1)

    struct TypeExtractionSymbols
    {
        TSSymbol symPrimitiveType = 0;
        TSSymbol symDatatype = 0;
        TSSymbol symTemplateTypeList = 0;
        TSSymbol symIdentifier = 0;
        TSSymbol symScopedType = 0;
        TSSymbol symType = 0;
        TSSymbol symNullLiteral = 0;
        TSSymbol symParenthesizedExpression = 0;

        ankerl::unordered_dense::map<TSSymbol, TypeKind> primitiveKindMap;

        TypeExtractionSymbols()
        {
            const TSLanguage *lang = tree_sitter_angelscript();

            symPrimitiveType = ts_language_symbol_for_name(lang, SYM_NAME("primitive_type"), true);
            symDatatype = ts_language_symbol_for_name(lang, SYM_NAME("datatype"), true);
            symTemplateTypeList = ts_language_symbol_for_name(lang, SYM_NAME("template_type_list"), true);
            symIdentifier = ts_language_symbol_for_name(lang, SYM_NAME("identifier"), true);
            symScopedType = ts_language_symbol_for_name(lang, SYM_NAME("scoped_type"), true);
            symType = ts_language_symbol_for_name(lang, SYM_NAME("type"), true);
            symNullLiteral = ts_language_symbol_for_name(lang, SYM_NAME("null_literal"), true);
            symParenthesizedExpression = ts_language_symbol_for_name(lang, SYM_NAME("parenthesized_expression"), true);

            auto addPrimitive = [&](const char *name, uint32_t len, TypeKind kind)
            {
                TSSymbol sym = ts_language_symbol_for_name(lang, name, len, false);
                if (sym != 0)
                {
                    primitiveKindMap.emplace(sym, kind);
                }
            };

            addPrimitive("void", 4, TypeKind::Void);
            addPrimitive("int", 3, TypeKind::Int32);
            addPrimitive("int32", 5, TypeKind::Int32);
            addPrimitive("int8", 4, TypeKind::Int8);
            addPrimitive("int16", 5, TypeKind::Int16);
            addPrimitive("int64", 5, TypeKind::Int64);
            addPrimitive("uint", 4, TypeKind::UInt32);
            addPrimitive("uint32", 6, TypeKind::UInt32);
            addPrimitive("uint8", 5, TypeKind::UInt8);
            addPrimitive("uint16", 6, TypeKind::UInt16);
            addPrimitive("uint64", 6, TypeKind::UInt64);
            addPrimitive("float", 5, TypeKind::Float);
            addPrimitive("double", 6, TypeKind::Double);
            addPrimitive("bool", 4, TypeKind::Bool);
            addPrimitive("string", 6, TypeKind::String);
            addPrimitive("auto", 4, TypeKind::Auto);
        }

        TypeKind LookupPrimitiveKind(TSSymbol symbol) const
        {
            auto it = primitiveKindMap.find(symbol);
            if (it != primitiveKindMap.end())
            {
                return it->second;
            }
            return TypeKind::Unknown;
        }
    };

    static const TypeExtractionSymbols &GetTypeExtractionSymbols()
    {
        static const TypeExtractionSymbols symbols;
        return symbols;
    }

    static std::string GetNodeText(TSNode node, const std::string &sourceCode)
    {
        if (ts_node_is_null(node))
            return "";

        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);

        if (start >= end || end > sourceCode.size())
            return "";

        return sourceCode.substr(start, end - start);
    }

    static std::string_view GetNodeView(TSNode node, const std::string &sourceCode)
    {
        if (ts_node_is_null(node))
            return {};

        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);

        if (start >= end || end > sourceCode.size())
            return {};

        return std::string_view(sourceCode.data() + start, end - start);
    }

    TypeExtractionResult ExtractTypeInfoFromAST(TSNode typeNode, const std::string &sourceCode)
    {
        TypeExtractionResult result;
        if (ts_node_is_null(typeNode))
            return result;

        const auto &symbols = GetTypeExtractionSymbols();
        TSSymbol nodeSymbol = ts_node_symbol(typeNode);

        if (nodeSymbol == symbols.symPrimitiveType)
        {
            result.baseTypeName = GetNodeText(typeNode, sourceCode);
            TSNode parentDatatype = ts_node_parent(typeNode);
            if (!ts_node_is_null(parentDatatype))
            {
                TSNode innerToken = ts_node_child(typeNode, 0);
                if (!ts_node_is_null(innerToken))
                {
                    result.kind = symbols.LookupPrimitiveKind(ts_node_symbol(innerToken));
                }
            }
            if (result.kind == TypeKind::Unknown)
            {
                result.kind = symbols.LookupPrimitiveKind(nodeSymbol);
            }
            return result;
        }

        if (nodeSymbol == symbols.symIdentifier || nodeSymbol == symbols.symScopedType)
        {
            result.baseTypeName = GetNodeText(typeNode, sourceCode);
            result.kind = TypeKind::Unknown;
            return result;
        }

        std::string datatypeText;
        uint32_t count = ts_node_child_count(typeNode);
        TSNode prevChild = ts_node_child(typeNode, 0);

        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(typeNode, i);
            TSSymbol childSym = ts_node_symbol(child);

            if (childSym == symbols.symScopedType)
            {
                result.baseTypeName = GetNodeText(child, sourceCode);
                result.kind = TypeKind::Unknown;
                datatypeText = result.baseTypeName;
                prevChild = child;
            }
            else if (childSym == symbols.symDatatype)
            {
                TSNode inner = ts_node_named_child(child, 0);
                if (!ts_node_is_null(inner))
                {
                    TypeExtractionResult innerInfo = ExtractTypeInfoFromAST(inner, sourceCode);
                    result.baseTypeName = innerInfo.baseTypeName;
                    result.kind = innerInfo.kind;
                    datatypeText = innerInfo.baseTypeName;
                }
                else
                {
                    datatypeText = GetNodeText(child, sourceCode);
                    result.baseTypeName = datatypeText;
                }
                prevChild = child;
            }
            else if (childSym == symbols.symTemplateTypeList)
            {
                result.isArray = true;
                result.arrayDepth++;
                result.templateName = datatypeText;

                uint32_t tCount = ts_node_named_child_count(child);
                for (uint32_t t = 0; t < tCount; ++t)
                {
                    TSNode innerType = ts_node_named_child(child, t);
                    if (ts_node_symbol(innerType) == symbols.symType)
                    {
                        TypeExtractionResult inner = ExtractTypeInfoFromAST(innerType, sourceCode);
                        result.templateArguments.push_back(inner);
                        if (result.templateArguments.size() == 1)
                        {
                            result.baseTypeName = inner.baseTypeName;
                            result.isHandle = inner.isHandle || result.isHandle;
                            result.hasPrimitiveHandle = inner.hasPrimitiveHandle;
                            result.arrayDepth += inner.arrayDepth;
                            if (inner.kind != TypeKind::Unknown)
                            {
                                result.kind = inner.kind;
                            }
                        }
                    }
                }
                prevChild = child;
            }
            else if (!ts_node_is_named(child))
            {
                std::string_view tok = GetNodeView(child, sourceCode);
                if (tok == "[")
                {
                    result.isArray = true;
                    result.arrayDepth++;
                }
                else if (tok == "@")
                {
                    std::string_view prevTok = !ts_node_is_null(prevChild) ? GetNodeView(prevChild, sourceCode) : "";
                    if (result.isHandle && prevTok != "const")
                    {
                        // Double handle @@ or @ @ without 'const' in between is not allowed in AngelScript
                        result.hasPrimitiveHandle = true;
                    }
                    result.isHandle = true;
                    if (!result.isArray && !ts_node_is_null(prevChild) && ts_node_symbol(prevChild) == symbols.symDatatype)
                    {
                        TSNode innerChild = ts_node_named_child(prevChild, 0);
                        if (!ts_node_is_null(innerChild) && ts_node_symbol(innerChild) == symbols.symPrimitiveType)
                        {
                            result.hasPrimitiveHandle = true;
                        }
                    }
                }
                else if (tok == "&")
                {
                    result.isReference = true;
                }
                else if (tok == "const")
                {
                    result.isConst = true;
                }
            }
            prevChild = child;
        }

        if (result.isArray)
        {
            result.kind = TypeKind::Array;
        }
        else if (result.isHandle && result.kind == TypeKind::Unknown)
        {
            result.kind = TypeKind::Handle;
        }

        return result;
    }

    bool IsNullInitializer(TSNode valueNode)
    {
        if (ts_node_is_null(valueNode))
            return false;

        const auto &symbols = GetTypeExtractionSymbols();
        TSNode node = valueNode;
        while (ts_node_symbol(node) == symbols.symParenthesizedExpression)
        {
            TSNode inner = ts_node_named_child(node, 0);
            if (ts_node_is_null(inner))
                break;
            node = inner;
        }
        return ts_node_symbol(node) == symbols.symNullLiteral;
    }
}
