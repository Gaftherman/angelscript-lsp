#pragma once

#include <string>
#include <string_view>
#include <ankerl/unordered_dense.h>
#include "analysis/DiagnosticContext.h"


namespace angel_lsp::analysis
{
    namespace node_types
    {
        constexpr std::string_view StringLiteral = "string_literal";
        constexpr std::string_view LambdaExpression = "lambda_expression";
        constexpr std::string_view BooleanLiteral = "boolean_literal";
        constexpr std::string_view NumberLiteral = "number_literal";
        constexpr std::string_view ImportDeclaration = "import_declaration";
        constexpr std::string_view NullLiteral = "null_literal";
        constexpr std::string_view CallExpression = "call_expression";
    }

    /**
     * @brief Classification categories for initializer list item expressions.
     */
    enum class InitializerItemKind
    {
        NumericOrExpression,
        StringLiteral,
        BooleanLiteral,
        NullLiteral,
        NestedInitializer
    };

    /**
     * @brief Checks whether the given name is a reserved AngelScript keyword.
     * @param name Symbol name to check.
     * @return True if name is a reserved keyword.
     */
    bool IsReservedKeyword(const std::string &name);

    /**
     * @brief Checks whether the given name is a primitive AngelScript type name.
     * @param name Symbol or type name to check.
     * @return True if name is a primitive type name.
     */
    bool IsPrimitiveTypeName(const std::string &name);

    /**
     * @brief Semantically classifies the token text of an initializer item.
     * @param item Token or expression text of the initializer item.
     * @return InitializerItemKind classification.
     */
    InitializerItemKind ClassifyInitializerItem(std::string_view item);

    /**
     * @brief Checks if a given base type name resolves to a mixin class symbol.
     * @param baseTypeName The name of the type to check.
     * @param table SymbolTable to look up the type.
     * @return True if baseTypeName resolves to a mixin class.
     */
    bool IsMixinClass(std::string_view baseTypeName, const class SymbolTable &table);

    /**
     * @brief Checks whether the given base type name is a known type (primitive, string, array, or in SymbolTable).
     * @param baseName Type base name to check.
     * @param ctx DiagnosticContext containing request and SymbolTable.
     * @return True if baseName is a known valid type.
     */
    bool IsKnownType(const std::string &baseName, const DiagnosticContext &ctx);
}

