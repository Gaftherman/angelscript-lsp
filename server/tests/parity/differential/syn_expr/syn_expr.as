// ==== case: array_index_read_write ====
// @expect: accept
void main() {
    array<int> a = {10, 20, 30};
    int x = a[0];
    a[1] = x + 5;
}

// ==== case: bad_array_index_unclosed ====
// @expect: reject
void main() {
    array<int> a = {1, 2, 3};
    int x = a[0;
}

// ==== case: bad_bitwise_on_float ====
// @expect: reject
void main() {
    float a = 1.5f;
    float b = 2.5f;
    float c = a & b;
}

// ==== case: bad_cast_missing_angle ====
// @expect: reject
class Base {}
void main() {
    Base b;
    Base@ b2 = cast(Base)(b);
}

// ==== case: bad_cast_primitive_handle ====
// @expect: reject
void main() {
    int x = 1;
    int@ p = cast<int>(x);
}

// ==== case: bad_cast_unrelated_class ====
// @expect: accept
class Alpha { int a; }
class Beta { int b; }
void main() {
    Alpha a;
    Beta@ b = cast<Beta>(a);
}

// ==== case: bad_compound_assign_const ====
// @expect: reject
void main() {
    const int x = 5;
    x += 1;
}

// ==== case: bad_compound_assign_rvalue ====
// @expect: reject
void main() {
    int a = 1;
    (a + 1) += 2;
}

// ==== case: bad_conv_const_handle_to_mutable ====
// @expect: reject
class Item { int id; }
void main() {
    const Item@ a = Item();
    Item@ b = a;
}

// ==== case: bad_conv_string_to_int ====
// @expect: reject
void main() {
    string s = "123";
    int x = s;
}

// ==== case: bad_init_list_on_scalar ====
// @expect: reject
void main() {
    int x = {1, 2};
}

// ==== case: bad_init_list_wrong_elem_type ====
// @expect: reject
void main() {
    array<int> arr = {1, "two", 3};
}

// ==== case: bad_is_on_value_class ====
// @expect: accept
class Box { int v; }
void main() {
    Box a;
    Box b;
    bool r = a !is b;
}

// ==== case: bad_null_assign_to_int ====
// @expect: reject
void main() {
    int x = null;
}

// ==== case: bad_null_compare_int ====
// @expect: reject
void main() {
    int x = 0;
    if (x == null) {}
}

// ==== case: bad_prec_assign_to_expr ====
// @expect: reject
void main() {
    int a = 1;
    int b = 2;
    (a + b) = 3;
}

// ==== case: bad_string_subtract ====
// @expect: reject
void main() {
    string s = "abc" - "a";
}

// ==== case: bad_ternary_mismatched_types ====
// @expect: reject
void main() {
    bool cond = true;
    int x = cond ? 1 : "two";
}

// ==== case: bad_ternary_non_bool_cond ====
// @expect: reject
void main() {
    int cond = 1;
    int x = cond ? 10 : 20;
}

// ==== case: bitwise_ops_uint ====
// @expect: accept
void main() {
    uint a = 0xF0;
    uint b = 0x0F;
    uint c = (a & b) | (a ^ b);
    uint d = ~a;
    uint e = a << 2;
    uint f = a >> 1;
    uint g = a >>> 1;
}

// ==== case: cast_handle_up_down ====
// @expect: accept
class Base { int v; }
class Derived : Base { int w; }
void main() {
    Derived d;
    Base@ b = cast<Base>(d);
    Derived@ d2 = cast<Derived>(b);
}

// ==== case: cast_primitive_int_float ====
// @expect: accept
void main() {
    float f = 3.5f;
    int i = int(f);
    double d = double(i);
}

// ==== case: compound_assign_arith ====
// @expect: accept
void main() {
    int x = 10;
    x += 5;
    x -= 2;
    x *= 3;
    x /= 4;
    x %= 3;
}

// ==== case: compound_assign_bitwise ====
// @expect: accept
void main() {
    uint x = 15;
    x &= 7;
    x |= 8;
    x ^= 3;
    x <<= 2;
    x >>= 1;
    x >>>= 1;
}

// ==== case: conv_enum_to_int ====
// @expect: accept
enum Color { RED = 1, GREEN = 2 }
void main() {
    Color c = RED;
    int x = c;
}

// ==== case: conv_handle_to_const_handle ====
// @expect: accept
class Item { int id; }
void main() {
    Item@ a = Item();
    const Item@ b = a;
}

// ==== case: conv_int_to_float ====
// @expect: accept
void main() {
    int i = 5;
    float f = i;
    double d = f;
}

// ==== case: init_list_array_int ====
// @expect: accept
void main() {
    array<int> arr = {1, 2, 3, 4};
}

// ==== case: init_list_array_string ====
// @expect: accept
void main() {
    array<string> names = {"a", "b", "c"};
}

// ==== case: init_list_nested_array ====
// @expect: accept
void main() {
    array<array<int>> mat = {{1, 2}, {3, 4}};
}

// ==== case: is_handle_identity ====
// @expect: accept
class Node { int v; }
void main() {
    Node@ a = Node();
    Node@ b = a;
    bool same = a is b;
    bool diff = a !is null;
}

// ==== case: is_null_literal ====
// @expect: accept
class Node {}
void main() {
    Node@ n = null;
    if (n is null) {}
    if (null !is n) {}
}

// ==== case: logical_ops_bool ====
// @expect: accept
void main() {
    bool a = true;
    bool b = false;
    bool c = (a && !b) || (a ^^ b);
}

// ==== case: null_assign_to_handle ====
// @expect: accept
class Foo {}
void main() {
    Foo@ f = Foo();
    @f = null;
}

// ==== case: null_check_if_handle ====
// @expect: accept
class Foo {}
void main() {
    Foo@ f = null;
    if (f !is null) {}
    if (f is null) {}
}

// ==== case: prec_arith_mul_add ====
// @expect: accept
void main() {
    int x = 1 + 2 * 3;
    int y = (1 + 2) * 3;
}

// ==== case: prec_logical_and_or ====
// @expect: accept
void main() {
    bool a = true || false && false;
    bool b = (true || false) && false;
}

// ==== case: prec_shift_vs_add ====
// @expect: accept
void main() {
    int x = 1 + 2 << 3;
    int y = 16 >> 1 + 1;
}

// ==== case: prec_unary_neg_vs_mul ====
// @expect: accept
void main() {
    int x = -2 * 3;
    bool y = !false && true;
}

// ==== case: string_concat_basic ====
// @expect: accept
void main() {
    string s = "hello" + " " + "world";
    s += "!";
}

// ==== case: string_concat_with_int ====
// @expect: accept
void main() {
    int n = 42;
    string s = "value=" + n;
}

// ==== case: ternary_basic_int ====
// @expect: accept
void main() {
    bool cond = true;
    int x = cond ? 10 : 20;
}

// ==== case: ternary_handle_and_null ====
// @expect: accept
class Obj {}
void main() {
    bool cond = true;
    Obj@ o = cond ? Obj() : null;
}

// ==== case: ternary_numeric_promote ====
// @expect: accept
void main() {
    bool cond = false;
    double x = cond ? 1 : 2.5;
}

// ==== case: bad_array_index_on_int ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 14, "Type 'int' doesn't support the indexing operator")]
// @expect: reject
void main() {
    int x = 5;
    int y = x[0];
}

// ==== case: bad_compound_assign_bool ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 7, "No conversion from 'bool' to math type available.")]
// @expect: reject
void main() {
    bool b = true;
    b += false;
}

// ==== case: bad_is_on_value_int ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(5, 16, 'Both operands must be handles when comparing identity')]
// @expect: reject
void main() {
    int a = 1;
    int b = 1;
    bool r = a is b;
}

// ==== case: bad_logical_on_int ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(5, 16, "No conversion from 'int' to 'bool' available.")]
// @expect: reject
void main() {
    int a = 1;
    int b = 2;
    bool c = a && b;
}
