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

; Class member variables are properties, not plain variables. Anchored to class_body, which is
; also what distinguishes them in LOCALS_QUERY.
(class_body
  (variable_declaration
    (variable_declarator
      name: (identifier) @property)))

; Parameter names
(parameter name: (identifier) @variable.parameter)

; Foreach variable names
(foreach_variable name: (identifier) @variable)

; Namespace.
;
; The qualifier pattern is anchored to identifiers immediately followed by "::" on purpose.
; scoped_identifier is not only used for qualified names: the grammar wraps EVERY bare
; identifier expression in one, so "f = 1;" parses as (scoped_identifier (identifier)). An
; unanchored "(scoped_identifier (identifier)) @module" therefore matched every variable read,
; parameter read and unqualified call in the language, and at priority 6 it outranked all of
; them - which is why plain identifiers were being painted as namespaces.
(namespace_declaration name: (scoped_identifier) @module)
(scoped_identifier (identifier) @module . "::")

; Qualifiers of a qualified TYPE take a different shape: "Weapons::Rifle r;" parses the
; namespace part into a (scope (scoped_identifier ...) "::") node, so the "::" is a sibling of
; the scoped_identifier rather than a child of it and the anchor above cannot see it. Every
; identifier under a scope node is a qualifier by construction.
(scope (scoped_identifier (identifier) @module))

; The trailing anchor picks the final name of a scoped_identifier: the actual thing being
; referred to, as opposed to the namespaces qualifying it. Calls override this through
; @function.call below, which is captured on the same node at a higher priority.
(scoped_identifier (identifier) @variable .)


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

; Function calls. Captured on the final identifier rather than the whole scoped_identifier so
; that a qualified call colours the callee and leaves the qualifier to the namespace pattern.
(call_expression function: (scoped_identifier (identifier) @function.call .))
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

; Variables (locals and module/namespace-scope globals).
;
; Anchored to every context a variable_declaration can appear in EXCEPT class_body, which is
; covered by the @local.definition.field pattern below. An unanchored
; "(variable_declarator name: (identifier))" also matches class fields, so every field used to be
; captured twice - once as a variable and once as a field - and downstream code had to
; disambiguate by walking ancestors to work out which capture was the real one. Enumerating the
; contexts here removes the ambiguity at the source; tree-sitter queries cannot express "not
; inside a class_body" directly, so the list is explicit and has to track the grammar:
; script, namespace_body, statement_block, for_statement init, and case_clause.
(script
  (variable_declaration
    (variable_declarator
      name: (identifier) @local.definition.var)))

(namespace_body
  (variable_declaration
    (variable_declarator
      name: (identifier) @local.definition.var)))

(statement_block
  (variable_declaration
    (variable_declarator
      name: (identifier) @local.definition.var)))

(for_statement
  init: (variable_declaration
    (variable_declarator
      name: (identifier) @local.definition.var)))

(case_clause
  (variable_declaration
    (variable_declarator
      name: (identifier) @local.definition.var)))


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
