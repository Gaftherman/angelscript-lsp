#pragma once

#include <array>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{

/**
 * @brief Cached Tree-Sitter grammar symbols used across parser, analysis, and feature passes.
 */
struct GrammarSymbols
{
    // =========================================================================
    // Named Node Types (sym*)
    // =========================================================================

    // Containers & Structure
    TSSymbol symScript = 0;               ///< script
    TSSymbol symClassDeclaration = 0;     ///< class_declaration
    TSSymbol symClassBody = 0;            ///< class_body
    TSSymbol symInterfaceDeclaration = 0; ///< interface_declaration
    TSSymbol symInterfaceBody = 0;        ///< interface_body
    TSSymbol symNamespaceDeclaration = 0; ///< namespace_declaration
    TSSymbol symNamespaceBody = 0;        ///< namespace_body
    TSSymbol symMixinDeclaration = 0;     ///< mixin_declaration
    TSSymbol symEnumDeclaration = 0;      ///< enum_declaration
    TSSymbol symEnumMember = 0;           ///< enum_member
    TSSymbol symBaseClassList = 0;        ///< base_class_list
    TSSymbol symScope = 0;                ///< scope

    // Declarations & Modifiers
    TSSymbol symFuncDeclaration = 0;        ///< func_declaration
    TSSymbol symFuncdefDeclaration = 0;     ///< funcdef_declaration
    TSSymbol symInterfaceMethod = 0;        ///< interface_method
    TSSymbol symVariableDeclaration = 0;    ///< variable_declaration
    TSSymbol symVariableDeclarator = 0;     ///< variable_declarator
    TSSymbol symVirtualProperty = 0;        ///< virtual_property
    TSSymbol symAccessor = 0;               ///< accessor
    TSSymbol symFuncAttributes = 0;         ///< func_attributes
    TSSymbol symDeclarationModifier = 0;    ///< declaration_modifier
    TSSymbol symSharedExternalModifier = 0; ///< shared_external_modifier
    TSSymbol symUsingDeclaration = 0;       ///< using_declaration
    TSSymbol symImportDeclaration = 0;      ///< import_declaration
    TSSymbol symTypedefDeclaration = 0;     ///< typedef_declaration
    TSSymbol symParameter = 0;              ///< parameter
    TSSymbol symParameterList = 0;          ///< parameter_list
    TSSymbol symForeachVariable = 0;        ///< foreach_variable
    TSSymbol symLambdaParameterList = 0;    ///< lambda_parameter_list

    // Types & Identifiers
    TSSymbol symIdentifier = 0;            ///< identifier
    TSSymbol symScopedIdentifier = 0;      ///< scoped_identifier
    TSSymbol symType = 0;                  ///< type
    TSSymbol symDatatype = 0;              ///< datatype
    TSSymbol symPrimitiveType = 0;         ///< primitive_type
    TSSymbol symNestedTypeName = 0;        ///< nested_type_name
    TSSymbol symTemplateTypeList = 0;      ///< template_type_list
    TSSymbol symTemplateParameterList = 0; ///< template_parameter_list

    // Expressions
    TSSymbol symMemberExpression = 0;         ///< member_expression
    TSSymbol symMemberAccess = 0;             ///< alias to member_expression for backwards-compat
    TSSymbol symCallExpression = 0;           ///< call_expression
    TSSymbol symArgumentList = 0;             ///< argument_list
    TSSymbol symCastExpression = 0;           ///< cast_expression
    TSSymbol symFunctionalCastExpression = 0; ///< functional_cast_expression
    TSSymbol symConstructCallExpression = 0;  ///< construct_call_expression
    TSSymbol symBinaryExpression = 0;         ///< binary_expression
    TSSymbol symAssignmentExpression = 0;     ///< assignment_expression
    TSSymbol symUnaryExpression = 0;          ///< unary_expression
    TSSymbol symPostfixExpression = 0;        ///< postfix_expression
    TSSymbol symParenthesizedExpression = 0;  ///< parenthesized_expression
    TSSymbol symTernaryExpression = 0;        ///< ternary_expression
    TSSymbol symIndexExpression = 0;          ///< index_expression
    TSSymbol symLambdaExpression = 0;         ///< lambda_expression
    TSSymbol symThisExpression = 0;           ///< this_expression
    TSSymbol symInitializerList = 0;          ///< initializer_list
    TSSymbol symTypedInitializerList = 0;     ///< typed_initializer_list

    // Literals & Directives
    TSSymbol symNullLiteral = 0;        ///< null_literal
    TSSymbol symBooleanLiteral = 0;     ///< boolean_literal
    TSSymbol symNumberLiteral = 0;      ///< number_literal
    TSSymbol symStringLiteral = 0;      ///< string_literal
    TSSymbol symConcatenatedString = 0; ///< concatenated_string
    TSSymbol symMetadata = 0;           ///< metadata
    TSSymbol symComment = 0;            ///< comment
    TSSymbol symPreprocDirective = 0;   ///< preproc_directive

    // Statements
    TSSymbol symStatementBlock = 0;      ///< statement_block
    TSSymbol symReturnStatement = 0;     ///< return_statement
    TSSymbol symBreakStatement = 0;      ///< break_statement
    TSSymbol symContinueStatement = 0;   ///< continue_statement
    TSSymbol symIfStatement = 0;         ///< if_statement
    TSSymbol symForStatement = 0;        ///< for_statement
    TSSymbol symForeachStatement = 0;    ///< foreach_statement
    TSSymbol symWhileStatement = 0;      ///< while_statement
    TSSymbol symDoWhileStatement = 0;    ///< do_while_statement
    TSSymbol symSwitchStatement = 0;     ///< switch_statement
    TSSymbol symCaseClause = 0;          ///< case_clause
    TSSymbol symTryStatement = 0;        ///< try_statement
    TSSymbol symExpressionStatement = 0; ///< expression_statement

    // =========================================================================
    // Anonymous Token Symbols (tok*)
    // =========================================================================

    TSSymbol tokConst = 0;                         ///< "const"
    TSSymbol tokIn = 0;                            ///< "in"
    TSSymbol tokOut = 0;                           ///< "out"
    TSSymbol tokInout = 0;                         ///< "inout"
    TSSymbol tokAmp = 0;                           ///< "&"
    TSSymbol tokAt = 0;                            ///< "@"
    TSSymbol tokPrivate = 0;                       ///< "private"
    TSSymbol tokProtected = 0;                     ///< "protected"
    TSSymbol tokShared = 0;                        ///< "shared"
    TSSymbol tokMixin = 0;                         ///< "mixin"
    TSSymbol tokAbstract = 0;                      ///< "abstract"
    TSSymbol tokFinal = 0;                         ///< "final"
    TSSymbol tokOverride = 0;                      ///< "override"
    TSSymbol tokExplicit = 0;                      ///< "explicit"
    TSSymbol tokProperty = 0;                      ///< "property"
    TSSymbol tokDelete = 0;                        ///< "delete"
    TSSymbol tokExternal = 0;                      ///< "external"
    TSSymbol tokImport = 0;                        ///< "import"
    TSSymbol tokOpenBrace = 0;                     ///< "{"
    TSSymbol tokCloseBrace = 0;                    ///< "}"
    TSSymbol tokGet = 0;                           ///< "get"
    TSSymbol tokSet = 0;                           ///< "set"
    TSSymbol tokColonColon = 0;                    ///< "::"
    TSSymbol tokClass = 0;                         ///< "class"
    TSSymbol tokInterface = 0;                     ///< "interface"
    TSSymbol tokFunction = 0;                      ///< "function"
    TSSymbol tokComma = 0;                         ///< ","
    TSSymbol tokOpenParen = 0;                     ///< "("
    TSSymbol tokCloseParen = 0;                    ///< ")"
    TSSymbol tokColon = 0;                         ///< ":"
    TSSymbol tokOpenBracket = 0;                   ///< "["
    TSSymbol tokCloseBracket = 0;                  ///< "]"
    TSSymbol tokAssign = 0;                        ///< "="
    TSSymbol tokSemicolon = 0;                     ///< ";"
    TSSymbol tokDefault = 0;                       ///< "default"
    TSSymbol tokCase = 0;                          ///< "case"
    TSSymbol tokDot = 0;                           ///< "."
    TSSymbol tokSlash = 0;                         ///< "/"
    TSSymbol symError = static_cast<TSSymbol>(-1); ///< ERROR node

    // =========================================================================
    // Pre-resolved Field IDs (field*)
    // =========================================================================

    TSFieldId fieldName = 0;           ///< "name"
    TSFieldId fieldType = 0;           ///< "type"
    TSFieldId fieldReturnType = 0;     ///< "return_type"
    TSFieldId fieldParameters = 0;     ///< "parameters"
    TSFieldId fieldBody = 0;           ///< "body"
    TSFieldId fieldFunction = 0;       ///< "function"
    TSFieldId fieldArguments = 0;      ///< "arguments"
    TSFieldId fieldObject = 0;         ///< "object"
    TSFieldId fieldMember = 0;         ///< "member"
    TSFieldId fieldLeft = 0;           ///< "left"
    TSFieldId fieldRight = 0;          ///< "right"
    TSFieldId fieldOperator = 0;       ///< "operator"
    TSFieldId fieldCondition = 0;      ///< "condition"
    TSFieldId fieldValue = 0;          ///< "value"
    TSFieldId fieldIndex = 0;          ///< "index"
    TSFieldId fieldIndexName = 0;      ///< "index_name"
    TSFieldId fieldArgName = 0;        ///< "arg_name"
    TSFieldId fieldHandler = 0;        ///< "handler"
    TSFieldId fieldVarType = 0;        ///< "var_type"
    TSFieldId fieldKind = 0;           ///< "kind"
    TSFieldId fieldSource = 0;         ///< "source"
    TSFieldId fieldPropType = 0;       ///< "prop_type"
    TSFieldId fieldParamType = 0;      ///< "param_type"
    TSFieldId fieldDefaultValue = 0;   ///< "default_value"
    TSFieldId fieldBaseType = 0;       ///< "base_type"
    TSFieldId fieldTemplateParams = 0; ///< "template_params"
    TSFieldId fieldUpdate = 0;         ///< "update"
    TSFieldId fieldInit = 0;           ///< "init"
    TSFieldId fieldConsequence = 0;    ///< "consequence"
    TSFieldId fieldOperand = 0;        ///< "operand"
    TSFieldId fieldAlternative = 0;    ///< "alternative"
    TSFieldId fieldBase = 0;           ///< "base"
    TSFieldId fieldCollection = 0;     ///< "collection"
    TSFieldId fieldModifier = 0;       ///< "modifier"
    TSFieldId fieldParam = 0;          ///< "param"
    TSFieldId fieldUnderlyingType = 0; ///< "underlying_type"

    // =========================================================================
    // Pre-resolved Statement & Expression Sets
    // =========================================================================

    /** @brief Pre-resolved target node symbols for single-pass type conversion checks. */
    std::array<TSSymbol, 19> conversionTargetSymbols{};
};

/**
 * @brief Initializes and returns statically cached Tree-Sitter grammar symbols.
 * @return Const reference to statically initialized grammar symbols.
 */
const GrammarSymbols& GetGrammarSymbols() noexcept;

} // namespace angel_lsp::parser
