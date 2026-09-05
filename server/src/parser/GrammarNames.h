#pragma once

#include <cstdint>
#include <string_view>

#include <tree_sitter/api.h>

/**
 * @file
 * @brief Every node type and field name the grammar defines, as constants.
 *
 * Generated from the grammar's own `src/node-types.json` - the names here are copied from
 * tree-sitter-angelscript, not typed out. That is the whole point.
 *
 * Before this header there was nothing checking that a name written in C++ existed in the grammar,
 * and twenty did not. `if (nodeType == "function_definition")` compiles, runs, and is false
 * forever; `ts_node_child_by_field_name(node, "initializer", 11)` compiles, runs, and returns null
 * forever. Most were names from the C, C++ and JavaScript grammars, copied in from another
 * project's handler and never true here. One - `function_declaration`, where this grammar says
 * `func_declaration` - was a live bug that stopped a search at classes and not at functions.
 *
 * Two guards keep it that way, and they answer different questions:
 *
 *   - tests/GrammarNamesTest.cpp asks the LOADED language whether each constant below still
 *     resolves. That is what turns a grammar pin bump into a failing test with a name in it,
 *     instead of a rule that quietly stops matching. The parity audit structurally cannot catch
 *     this: an unparseable construct costs a symbol rather than producing a diagnostic, so a
 *     grammar gap reaches it as silence. See PARITY-BACKLOG.md.
 *
 *   - scripts/check-grammar-names.py asks the SOURCE whether anyone wrote a raw string literal in
 *     a node-type or field position that the grammar does not define. That is what catches a new
 *     name typed from memory, which is how all twenty arrived.
 *
 * Regenerate with scripts/gen-grammar-names.py after bumping the grammar pin in
 * cmake/TreeSitter.cmake.
 */
namespace angel_lsp::parser
{
    /** @brief Named node types. Anonymous tokens - "class", "int", "(" - are deliberately absent. */
    namespace nodes
    {
        inline constexpr std::string_view Accessor                     = "accessor";
        inline constexpr std::string_view ArgumentList                 = "argument_list";
        inline constexpr std::string_view AssignmentExpression         = "assignment_expression";
        inline constexpr std::string_view BaseClassList                = "base_class_list";
        inline constexpr std::string_view BinaryExpression             = "binary_expression";
        inline constexpr std::string_view BooleanLiteral               = "boolean_literal";
        inline constexpr std::string_view BreakStatement               = "break_statement";
        inline constexpr std::string_view CallExpression               = "call_expression";
        inline constexpr std::string_view CaseClause                   = "case_clause";
        inline constexpr std::string_view CastExpression               = "cast_expression";
        inline constexpr std::string_view ClassBody                    = "class_body";
        inline constexpr std::string_view ClassDeclaration             = "class_declaration";
        inline constexpr std::string_view Comment                      = "comment";
        inline constexpr std::string_view ConcatenatedString           = "concatenated_string";
        inline constexpr std::string_view ConstructCallExpression      = "construct_call_expression";
        inline constexpr std::string_view ContinueStatement            = "continue_statement";
        inline constexpr std::string_view Datatype                     = "datatype";
        inline constexpr std::string_view DeclarationModifier          = "declaration_modifier";
        inline constexpr std::string_view DoWhileStatement             = "do_while_statement";
        inline constexpr std::string_view EnumDeclaration              = "enum_declaration";
        inline constexpr std::string_view EnumMember                   = "enum_member";
        inline constexpr std::string_view ExpressionStatement          = "expression_statement";
        inline constexpr std::string_view ForStatement                 = "for_statement";
        inline constexpr std::string_view ForeachStatement             = "foreach_statement";
        inline constexpr std::string_view ForeachVariable              = "foreach_variable";
        inline constexpr std::string_view FuncAttributes               = "func_attributes";
        inline constexpr std::string_view FuncDeclaration              = "func_declaration";
        inline constexpr std::string_view FuncdefDeclaration           = "funcdef_declaration";
        inline constexpr std::string_view FunctionalCastExpression     = "functional_cast_expression";
        inline constexpr std::string_view Identifier                   = "identifier";
        inline constexpr std::string_view IfStatement                  = "if_statement";
        inline constexpr std::string_view ImportDeclaration            = "import_declaration";
        inline constexpr std::string_view IndexExpression              = "index_expression";
        inline constexpr std::string_view InitializerList              = "initializer_list";
        inline constexpr std::string_view InterfaceBody                = "interface_body";
        inline constexpr std::string_view InterfaceDeclaration         = "interface_declaration";
        inline constexpr std::string_view InterfaceMethod              = "interface_method";
        inline constexpr std::string_view LambdaExpression             = "lambda_expression";
        inline constexpr std::string_view LambdaParameterList          = "lambda_parameter_list";
        inline constexpr std::string_view MemberExpression             = "member_expression";
        inline constexpr std::string_view Metadata                     = "metadata";
        inline constexpr std::string_view MixinDeclaration             = "mixin_declaration";
        inline constexpr std::string_view NamespaceBody                = "namespace_body";
        inline constexpr std::string_view NamespaceDeclaration         = "namespace_declaration";
        inline constexpr std::string_view NestedTypeName               = "nested_type_name";
        inline constexpr std::string_view NullLiteral                  = "null_literal";
        inline constexpr std::string_view NumberLiteral                = "number_literal";
        inline constexpr std::string_view Parameter                    = "parameter";
        inline constexpr std::string_view ParameterList                = "parameter_list";
        inline constexpr std::string_view ParenthesizedExpression      = "parenthesized_expression";
        inline constexpr std::string_view PostfixExpression            = "postfix_expression";
        inline constexpr std::string_view PreprocDirective             = "preproc_directive";
        inline constexpr std::string_view PrimitiveType                = "primitive_type";
        inline constexpr std::string_view ReturnStatement              = "return_statement";
        inline constexpr std::string_view Scope                        = "scope";
        inline constexpr std::string_view ScopedIdentifier             = "scoped_identifier";
        inline constexpr std::string_view Script                       = "script";
        inline constexpr std::string_view SharedExternalModifier       = "shared_external_modifier";
        inline constexpr std::string_view StatementBlock               = "statement_block";
        inline constexpr std::string_view StringLiteral                = "string_literal";
        inline constexpr std::string_view SwitchStatement              = "switch_statement";
        inline constexpr std::string_view TemplateParameterList        = "template_parameter_list";
        inline constexpr std::string_view TemplateTypeList             = "template_type_list";
        inline constexpr std::string_view TernaryExpression            = "ternary_expression";
        inline constexpr std::string_view ThisExpression               = "this_expression";
        inline constexpr std::string_view TryStatement                 = "try_statement";
        inline constexpr std::string_view Type                         = "type";
        inline constexpr std::string_view TypedInitializerList         = "typed_initializer_list";
        inline constexpr std::string_view TypedefDeclaration           = "typedef_declaration";
        inline constexpr std::string_view UnaryExpression              = "unary_expression";
        inline constexpr std::string_view UsingDeclaration             = "using_declaration";
        inline constexpr std::string_view VariableDeclaration          = "variable_declaration";
        inline constexpr std::string_view VariableDeclarator           = "variable_declarator";
        inline constexpr std::string_view VirtualProperty              = "virtual_property";
        inline constexpr std::string_view WhileStatement               = "while_statement";
    }

    /** @brief Field names, for ts_node_child_by_field_name and the helper below. */
    namespace fields
    {
        inline constexpr std::string_view Alternative          = "alternative";
        inline constexpr std::string_view ArgName              = "arg_name";
        inline constexpr std::string_view Arguments            = "arguments";
        inline constexpr std::string_view Base                 = "base";
        inline constexpr std::string_view BaseType             = "base_type";
        inline constexpr std::string_view Body                 = "body";
        inline constexpr std::string_view Collection           = "collection";
        inline constexpr std::string_view Condition            = "condition";
        inline constexpr std::string_view Consequence          = "consequence";
        inline constexpr std::string_view DefaultValue         = "default_value";
        inline constexpr std::string_view Function             = "function";
        inline constexpr std::string_view Index                = "index";
        inline constexpr std::string_view IndexName            = "index_name";
        inline constexpr std::string_view Init                 = "init";
        inline constexpr std::string_view Kind                 = "kind";
        inline constexpr std::string_view Left                 = "left";
        inline constexpr std::string_view Member               = "member";
        inline constexpr std::string_view Modifier             = "modifier";
        inline constexpr std::string_view Name                 = "name";
        inline constexpr std::string_view Object               = "object";
        inline constexpr std::string_view Operand              = "operand";
        inline constexpr std::string_view Operator             = "operator";
        inline constexpr std::string_view Param                = "param";
        inline constexpr std::string_view ParamType            = "param_type";
        inline constexpr std::string_view Parameters           = "parameters";
        inline constexpr std::string_view PropType             = "prop_type";
        inline constexpr std::string_view ReturnType           = "return_type";
        inline constexpr std::string_view Right                = "right";
        inline constexpr std::string_view Source               = "source";
        inline constexpr std::string_view TemplateParams       = "template_params";
        inline constexpr std::string_view Type                 = "type";
        inline constexpr std::string_view UnderlyingType       = "underlying_type";
        inline constexpr std::string_view Update               = "update";
        inline constexpr std::string_view Value                = "value";
        inline constexpr std::string_view VarType              = "var_type";
    }

    /**
     * @brief ts_node_child_by_field_name with the length taken from the name itself.
     *
     * The reason this exists is that 221 of the 344 field lookups in this server passed the length
     * as a separate integer literal - `"consequence", 11` - and 31 more declared a per-file
     * `k_consequenceFieldLength` constant to say the same thing. All of them happened to be
     * correct; none of them had to be.
     */
    [[nodiscard]] inline TSNode GetChildByField(TSNode parent, std::string_view fieldName) noexcept
    {
        if (ts_node_is_null(parent))
        {
            return TSNode{};
        }
        return ts_node_child_by_field_name(parent, fieldName.data(),
                                           static_cast<uint32_t>(fieldName.length()));
    }

    /** @brief Every constant in nodes::, for the test that checks they still resolve. */
    inline constexpr std::string_view k_allNodeTypes[] = {
        nodes::Accessor,
        nodes::ArgumentList,
        nodes::AssignmentExpression,
        nodes::BaseClassList,
        nodes::BinaryExpression,
        nodes::BooleanLiteral,
        nodes::BreakStatement,
        nodes::CallExpression,
        nodes::CaseClause,
        nodes::CastExpression,
        nodes::ClassBody,
        nodes::ClassDeclaration,
        nodes::Comment,
        nodes::ConcatenatedString,
        nodes::ConstructCallExpression,
        nodes::ContinueStatement,
        nodes::Datatype,
        nodes::DeclarationModifier,
        nodes::DoWhileStatement,
        nodes::EnumDeclaration,
        nodes::EnumMember,
        nodes::ExpressionStatement,
        nodes::ForStatement,
        nodes::ForeachStatement,
        nodes::ForeachVariable,
        nodes::FuncAttributes,
        nodes::FuncDeclaration,
        nodes::FuncdefDeclaration,
        nodes::FunctionalCastExpression,
        nodes::Identifier,
        nodes::IfStatement,
        nodes::ImportDeclaration,
        nodes::IndexExpression,
        nodes::InitializerList,
        nodes::InterfaceBody,
        nodes::InterfaceDeclaration,
        nodes::InterfaceMethod,
        nodes::LambdaExpression,
        nodes::LambdaParameterList,
        nodes::MemberExpression,
        nodes::Metadata,
        nodes::MixinDeclaration,
        nodes::NamespaceBody,
        nodes::NamespaceDeclaration,
        nodes::NestedTypeName,
        nodes::NullLiteral,
        nodes::NumberLiteral,
        nodes::Parameter,
        nodes::ParameterList,
        nodes::ParenthesizedExpression,
        nodes::PostfixExpression,
        nodes::PreprocDirective,
        nodes::PrimitiveType,
        nodes::ReturnStatement,
        nodes::Scope,
        nodes::ScopedIdentifier,
        nodes::Script,
        nodes::SharedExternalModifier,
        nodes::StatementBlock,
        nodes::StringLiteral,
        nodes::SwitchStatement,
        nodes::TemplateParameterList,
        nodes::TemplateTypeList,
        nodes::TernaryExpression,
        nodes::ThisExpression,
        nodes::TryStatement,
        nodes::Type,
        nodes::TypedInitializerList,
        nodes::TypedefDeclaration,
        nodes::UnaryExpression,
        nodes::UsingDeclaration,
        nodes::VariableDeclaration,
        nodes::VariableDeclarator,
        nodes::VirtualProperty,
        nodes::WhileStatement,
    };

    /** @brief Every constant in fields::, same purpose. */
    inline constexpr std::string_view k_allFieldNames[] = {
        fields::Alternative,
        fields::ArgName,
        fields::Arguments,
        fields::Base,
        fields::BaseType,
        fields::Body,
        fields::Collection,
        fields::Condition,
        fields::Consequence,
        fields::DefaultValue,
        fields::Function,
        fields::Index,
        fields::IndexName,
        fields::Init,
        fields::Kind,
        fields::Left,
        fields::Member,
        fields::Modifier,
        fields::Name,
        fields::Object,
        fields::Operand,
        fields::Operator,
        fields::Param,
        fields::ParamType,
        fields::Parameters,
        fields::PropType,
        fields::ReturnType,
        fields::Right,
        fields::Source,
        fields::TemplateParams,
        fields::Type,
        fields::UnderlyingType,
        fields::Update,
        fields::Value,
        fields::VarType,
    };
}
