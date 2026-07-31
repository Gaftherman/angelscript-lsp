// SA-07 edge cases: these should NOT generate as-err-unresolved-type

// typedef'd types — exist in SymbolTable
typedef int MyInt;
MyInt my_var;

// enum types — exist in SymbolTable
enum Color { Red = 1, Green = 2 }
Color c;

// auto — TypeKind::Auto, not Unknown
// auto x = 5;  // skip: tree-sitter may not parse this as variable_declaration at global scope

// handle of primitive — TypeKind::Handle (not Unknown), BUT hasPrimitiveHandle=true → different error
// int@ bad_handle;   // already tested elsewhere

// class that exists
class Player {}
Player p;

// array of unknown — typeKind=Array, not Unknown → should NOT trigger unresolved
// UnknownType[] arr;  // need to think about this

// SA-09: mixin that inherits another mixin — should be valid
mixin class MixinA {}
mixin class MixinB {}
// mixin class MixinC : MixinA {}  // is this valid in AngelScript?
