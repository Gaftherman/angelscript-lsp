; Tags for code navigation (e.g. :GoToSymbol in editors)

; Functions
(func_declaration
  name: (identifier) @name) @definition.function

; Classes
(class_declaration
  name: (identifier) @name) @definition.class

; Interfaces
(interface_declaration
  name: (identifier) @name) @definition.interface

; Mixin classes
(mixin_declaration
  name: (identifier) @name) @definition.class

; Enums
(enum_declaration
  name: (identifier) @name) @definition.enum

; Namespaces
(namespace_declaration
  name: (scoped_identifier) @name) @definition.module

; Typedefs
(typedef_declaration
  name: (identifier) @name) @definition.type

; Funcdefs (function type aliases)
(funcdef_declaration
  name: (identifier) @name) @definition.type

; Interface methods
(interface_method
  name: (identifier) @name) @definition.method

; Virtual properties
(virtual_property
  name: (identifier) @name) @definition.property

; Function calls (references)
(call_expression
  function: (identifier) @name) @reference.call

; Method calls (references)
(call_expression
  function: (member_expression
    member: (identifier) @name)) @reference.call
