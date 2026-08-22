#pragma once

namespace angel_lsp::parser::queries
{
    constexpr const char *HIGHLIGHTS_QUERY = R"SCM(
; Highlights for AngelScript

; Comments
(comment) @comment

; Preprocessor directives (#include, #if, #ifdef, #else, #endif, #pragma, ...)
(preproc_directive) @keyword.directive

; Literals
(string_literal) @string
(number_literal) @number

; Types
(primitive_type) @type.builtin
(datatype (identifier) @type)
(datatype "?" @type.builtin)

; Declaration names
(class_declaration name: (identifier) @type)
(interface_declaration name: (identifier) @type)
(enum_declaration name: (identifier) @type)
(typedef_declaration name: (identifier) @type)
(funcdef_declaration name: (identifier) @type)
(mixin_declaration name: (identifier) @type)
(base_class_list base: (scoped_identifier) @type)

; Enum members
(enum_member name: (identifier) @constant)

; Function names
(func_declaration name: (identifier) @function)
(interface_method name: (identifier) @function)
(import_declaration name: (identifier) @function)

; Property names
(virtual_property name: (identifier) @property)

; Variable names
(variable_declarator name: (identifier) @variable)

; Parameter names
(parameter name: (identifier) @variable.parameter)

; Foreach variable names
(foreach_variable name: (identifier) @variable)

; Namespace
(namespace_declaration name: (scoped_identifier) @module)
(scoped_identifier (identifier) @module)

; Keywords
[
  "class"
  "interface"
  "enum"
  "namespace"
  "import"
  "from"
  "using"
  "typedef"
  "funcdef"
  "mixin"
  "return"
] @keyword

; Control flow
[
  "if"
  "else"
  "for"
  "foreach"
  "while"
  "do"
  "switch"
  "case"
  "default"
  "try"
  "catch"
  "break"
  "continue"
] @keyword.control

; Modifiers
(declaration_modifier) @keyword.modifier
(shared_external_modifier) @keyword.modifier
[
  "private"
  "protected"
  "const"
  "override"
  "explicit"
  "property"
  "delete"
  "in"
  "out"
  "inout"
] @keyword.modifier

; Accessor keywords
(accessor "get" @keyword)
(accessor "set" @keyword)

; Type qualifiers
"auto" @type.builtin

; Punctuation
["(" ")" "{" "}" "[" "]"] @punctuation.bracket
[";" "," "." ":" "::"] @punctuation.delimiter
["&" "@" "~" "..."] @punctuation.special

; Boolean and null literals
(boolean_literal) @boolean
(null_literal) @constant.builtin

; Function calls
(call_expression function: (scoped_identifier) @function.call)
(call_expression function: (member_expression member: (identifier) @function.method.call))

; Named arguments
(argument_list arg_name: (identifier) @variable.parameter)

; Cast keyword
(cast_expression "cast" @keyword)

; Lambda keyword
(lambda_expression "function" @keyword)

; Ternary
(ternary_expression "?" @operator)
(ternary_expression ":" @operator)

; Operators
(binary_expression operator: _ @operator)
(assignment_expression operator: _ @operator)
(unary_expression operator: _ @operator)
(postfix_expression operator: _ @operator)
(typed_initializer_list "=" @operator)

; Index expression
(index_expression index_name: (identifier) @variable.parameter)
)SCM";

    constexpr const char *LOCALS_QUERY = R"SCM(
; Locals for AngelScript
; Scope tracking: definitions, references, and scopes for editors and
; downstream tools (local variable highlighting, smart rename).

; =============================================================================
; Scopes
; =============================================================================

(script) @local.scope

; Scopes are anchored on body nodes (not the declaration nodes) so the
; declared name itself stays visible in the enclosing scope.
; class_body also covers mixin_declaration.
(namespace_body) @local.scope
(class_body) @local.scope
(interface_body) @local.scope

; Functions scope their parameters together with the body,
; so the whole declaration is the scope (as in the C grammar).
(func_declaration) @local.scope
(lambda_expression) @local.scope

(statement_block) @local.scope

; Loop headers declare variables visible in the loop body
(for_statement) @local.scope
(foreach_statement) @local.scope

; case clauses may declare variables directly inside the switch
(switch_statement) @local.scope

; =============================================================================
; Definitions
; =============================================================================

; Parameters
(parameter
  name: (identifier) @local.definition.parameter)

; Lambda parameters. lambda_parameter_list does not wrap each entry in its own
; parameter node like a regular function/method - the name field sits directly
; on the list node, once per parameter - so it needs its own pattern.
(lambda_parameter_list
  name: (identifier) @local.definition.parameter)

; Variables (locals and globals)
(variable_declarator
  name: (identifier) @local.definition.var)

; Class member variables are fields
(class_body
  (variable_declaration
    (variable_declarator
      name: (identifier) @local.definition.field)))

; foreach loop variables
(foreach_variable
  name: (identifier) @local.definition.var)

; Functions and methods
(func_declaration
  name: (identifier) @local.definition.function)

(class_body
  (func_declaration
    name: (identifier) @local.definition.method))

(interface_method
  name: (identifier) @local.definition.method)

; Type declarations
(class_declaration
  name: (identifier) @local.definition.type)

(mixin_declaration
  name: (identifier) @local.definition.type)

(interface_declaration
  name: (identifier) @local.definition.type)

(enum_declaration
  name: (identifier) @local.definition.type)

(typedef_declaration
  name: (identifier) @local.definition.type)

(funcdef_declaration
  name: (identifier) @local.definition.type)

; Enum members
(enum_member
  name: (identifier) @local.definition.constant)

; Namespaces
(namespace_declaration
  name: (scoped_identifier
    (identifier) @local.definition.namespace))

; Virtual properties (get/set) behave like fields
(virtual_property
  name: (identifier) @local.definition.field)

; Imported functions: import void Func(...) from "module";
(import_declaration
  name: (identifier) @local.definition.import)

; =============================================================================
; References
; =============================================================================

(identifier) @local.reference
)SCM";

    constexpr const char *TAGS_QUERY = R"SCM(
; Tags for code navigation (e.g. :GoToSymbol in editors)

; Functions
(func_declaration) @definition.function

; Classes
(class_declaration) @definition.class

; Interfaces
(interface_declaration) @definition.interface

; Mixin classes
(mixin_declaration) @definition.class

; Enums
(enum_declaration) @definition.enum

; Namespaces
(namespace_declaration) @definition.namespace

; Typedefs
(typedef_declaration) @definition.typedef

; Funcdefs (function type aliases)
(funcdef_declaration) @definition.funcdef

; Interface methods
(interface_method) @definition.function

; Imported functions
(import_declaration) @definition.function

; Virtual properties
(virtual_property) @definition.property

; Variables
(variable_declaration) @definition.variable

; Function calls (references), including namespace-qualified calls (e.g. NS::Func()).
; The grammar always parses a call target without a member access as scoped_identifier,
; even for a plain unqualified call like Foo() - there is no bare (identifier) call target.
(call_expression
  function: (scoped_identifier) @name) @reference.call

; Method calls (references)
(call_expression
  function: (member_expression
    member: (identifier) @name)) @reference.call

; Using-declaration validation: flags `using namespace <reserved-keyword>;`.
(using_declaration) @validation.using

; Declaration-modifier duplicate validation. class_declaration/mixin_declaration
; expose "modifier" as declaration_modifier; interface_declaration exposes it as
; shared_external_modifier instead - the handler checks for both node types on
; whichever of these three declarations it is dispatched against.
[
  (class_declaration)
  (interface_declaration)
  (mixin_declaration)
] @validation.modifiers
)SCM";
}
