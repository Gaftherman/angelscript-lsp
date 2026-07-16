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
