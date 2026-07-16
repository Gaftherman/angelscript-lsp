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
(base_class_list base: (identifier) @type)

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
(call_expression function: (identifier) @function.call)
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

; Index expression
(index_expression index_name: (identifier) @variable.parameter)
