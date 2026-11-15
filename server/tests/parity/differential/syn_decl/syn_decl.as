// ==== case: array_bracket_decl ====
// @expect: accept
void f() {
    int[] a = {1, 2};
}

// ==== case: array_handle_elements ====
// @expect: accept
class Item {}
void f() {
    array<Item@> items;
}

// ==== case: array_template_decl ====
// @expect: accept
void f() {
    array<int> a;
}

// ==== case: auto_const_local ====
// @expect: accept
void f() {
    const auto x = 3.5;
}

// ==== case: auto_handle_local ====
// @expect: accept
class Node {}
void f() {
    Node@ n = Node();
    auto@ m = n;
}

// ==== case: auto_local_int ====
// @expect: accept
void f() {
    auto x = 42;
}

// ==== case: bad_array_empty_angle ====
// @expect: reject
void f() {
    array<> a;
}

// ==== case: bad_array_sized_bracket ====
// @expect: reject
void f() {
    int[4] a;
}

// ==== case: bad_array_void ====
// @expect: reject
void f() {
    array<void> a;
}

// ==== case: bad_auto_no_init_expr ====
// @expect: reject
void f() {
    auto x = ;
}

// ==== case: bad_auto_uninit ====
// @expect: reject
void f() {
    auto x;
}

// ==== case: bad_const_param_modify ====
// @expect: reject
void f(const int x) {
    x = 2;
}

// ==== case: bad_const_reassign ====
// @expect: reject
void f() {
    const int x = 1;
    x = 2;
}

// ==== case: bad_enum_dup_enumerator ====
// @expect: reject
enum Dup {
    A = 1,
    A = 2
}

// ==== case: bad_enum_string_val ====
// @expect: reject
enum E { A = "str" }

// ==== case: bad_funcdef_body ====
// @expect: reject
funcdef void Callback(int a) {}

// ==== case: bad_funcdef_no_semi ====
// @expect: reject
// @range: loose
funcdef void Callback(int a)

// ==== case: bad_handle_primitive ====
// @expect: reject
void f() {
    int@ p = null;
}

// ==== case: bad_handle_to_const_modify ====
// @expect: reject
class Item { int v; }
void f() {
    const Item@ p = Item();
    p.v = 1;
}

// ==== case: bad_local_out_of_scope ====
// @expect: reject
void f() {
    {
        int x = 1;
    }
    int y = x;
}

// ==== case: bad_shadow_param_in_body ====
// @expect: reject
void f(int x) {
    int x = 2;
}

// ==== case: bad_typedef_class ====
// @expect: reject
class Foo {}
typedef Foo Bar;

// ==== case: bad_typedef_no_semi ====
// @expect: reject
// @range: loose
typedef float real

// ==== case: bad_var_dup_global ====
// @expect: reject
int g = 1;
int g = 2;

// ==== case: bad_var_dup_local ====
// @expect: reject
void f() {
    int x = 1;
    int x = 2;
}

// ==== case: bad_var_empty_name ====
// @expect: reject
int ;

// ==== case: bad_var_missing_semi ====
// @expect: reject
// @range: loose
void f() {
    int x = 1
}

// ==== case: bad_var_unknown_type ====
// @expect: reject
void f() {
    UnknownType x;
}

// ==== case: bad_var_void_type ====
// @expect: reject
void x;

// ==== case: const_global_init ====
// @expect: accept
const int MAX = 100;

// ==== case: const_handle_to_const ====
// @expect: accept
class Item {}
void f() {
    const Item@ p = null;
}

// ==== case: const_local_init ====
// @expect: accept
void f() {
    const float PI = 3.14f;
}

// ==== case: enum_explicit_ints ====
// @expect: accept
enum Flags {
    None = 0,
    Read = 1,
    Write = 2,
    All = 3
}

// ==== case: enum_implicit_values ====
// @expect: accept
enum Color {
    Red,
    Green,
    Blue
}

// ==== case: funcdef_basic ====
// @expect: accept
funcdef void Callback(int code);

// ==== case: funcdef_handle_usage ====
// @expect: accept
funcdef int BinaryOp(int a, int b);
void f() {
    BinaryOp@ op = null;
}

// ==== case: handle_assign_op ====
// @expect: accept
class Obj {}
void f() {
    Obj a;
    Obj@ p = @a;
}

// ==== case: handle_const_handle ====
// @expect: accept
class Obj {}
void f() {
    Obj@ const p = null;
}

// ==== case: handle_null_init ====
// @expect: accept
class Obj {}
void f() {
    Obj@ o = null;
}

// ==== case: shadow_inner_block ====
// @expect: accept
void f() {
    int x = 1;
    if (true) {
        int x = 2;
    }
}

// ==== case: shadow_local_over_global ====
// @expect: accept
int x = 10;
void f() {
    int x = 20;
}

// ==== case: typedef_basic ====
// @expect: accept
typedef float real;
real x = 1.0f;

// ==== case: typedef_int_alias ====
// @expect: accept
typedef int my_int32;
void f() {
    my_int32 v = 5;
}

// ==== case: var_ctor_init ====
// @expect: accept
class Box {
    int x;
    Box(int v) { x = v; }
}
void f() {
    Box b(42);
}

// ==== case: var_global_init_expr ====
// @expect: accept
int g_a = 10;
int g_b = g_a + 5;

// ==== case: var_global_uninit ====
// @expect: accept
int g_count;

// ==== case: var_init_list_array ====
// @expect: accept
void f() {
    array<int> a = {1, 2, 3};
}

// ==== case: var_multi_decl ====
// @expect: accept
void f() {
    int a = 1, b = 2, c;
}

// ==== case: bad_auto_func_param ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(2, 8, 'Auto is not allowed here')]
// @expect: reject
void f(auto x) {}

// ==== case: bad_handle_double_at ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 9, 'Handle to handle is not allowed')]
// @expect: reject
class Obj {}
void f() {
    Obj@@ p = null;
}
