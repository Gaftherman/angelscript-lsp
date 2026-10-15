#pragma once

#include <tree_sitter/api.h>

namespace angel_lsp::parser
{

/**
 * @brief Cached Tree-Sitter grammar symbols used across parser, analysis, and feature passes.
 */
struct GrammarSymbols
{
    TSSymbol symTemplateTypeList = 0;        ///< template_type_list
    TSSymbol symCastExpression = 0;          ///< cast_expression
    TSSymbol symTemplateParameterList = 0;   ///< template_parameter_list
    TSSymbol symBinaryExpression = 0;        ///< binary_expression
    TSSymbol symAssignmentExpression = 0;    ///< assignment_expression
    TSSymbol symUnaryExpression = 0;         ///< unary_expression
    TSSymbol symPostfixExpression = 0;       ///< postfix_expression
    TSSymbol symParameter = 0;               ///< parameter
    TSSymbol symScopedIdentifier = 0;        ///< scoped_identifier
    TSSymbol symClassDeclaration = 0;        ///< class_declaration
    TSSymbol symType = 0;                    ///< type
    TSSymbol symDatatype = 0;                ///< datatype
    TSSymbol symScope = 0;                   ///< scope
    TSSymbol symMemberAccess = 0;            ///< member_expression
    TSSymbol symNullLiteral = 0;             ///< null_literal
    TSSymbol symParenthesizedExpression = 0; ///< parenthesized_expression
    TSSymbol symVariableDeclarator = 0;      ///< variable_declarator
    TSSymbol symForeachVariable = 0;         ///< foreach_variable
    TSSymbol symLambdaParameterList = 0;     ///< lambda_parameter_list
};

/**
 * @brief Initializes and returns statically cached Tree-Sitter grammar symbols.
 * @return Const reference to statically initialized grammar symbols.
 */
const GrammarSymbols& GetGrammarSymbols() noexcept;

} // namespace angel_lsp::parser
