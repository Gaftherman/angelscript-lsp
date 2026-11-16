// ==== case: prim_void_good ====
// @expect: accept
void doNothing() {}
void main() {
    doNothing();
}

// ==== case: prim_void_normal ====
// @expect: accept
void processValue(int x) {
    if (x < 0) return;
    int y = x * 2;
}
void noArgs(void) {}
void main() {
    processValue(10);
    noArgs();
}

// ==== case: prim_void_bad ====
// @expect: reject
void main() {
    void x;
}

// ==== case: prim_bool_good ====
// @expect: accept
void main() {
    bool b = true;
}

// ==== case: prim_bool_normal ====
// @expect: accept
void main() {
    bool a = true;
    bool b = false;
    bool c = (a && !b) || false;
    if (c) {
        b = true;
    }
}

// ==== case: prim_bool_bad ====
// @expect: reject
void main() {
    bool b = "true";
}

// ==== case: prim_int8_good ====
// @expect: accept
void main() {
    int8 x = 127;
}

// ==== case: prim_int8_normal ====
// @expect: accept
void main() {
    int8 a = 10;
    int8 b = -20;
    int8 c = a + b;
}

// ==== case: prim_int8_bad ====
// @expect: reject
void main() {
    int8@ p;
}

// ==== case: prim_int16_good ====
// @expect: accept
void main() {
    int16 x = 32767;
}

// ==== case: prim_int16_normal ====
// @expect: accept
void main() {
    int16 a = 1000;
    int16 b = 2;
    int16 c = a * b;
}

// ==== case: prim_int16_bad ====
// @expect: reject
void main() {
    int16 x = "hello";
}

// ==== case: prim_int32_good ====
// @expect: accept
void main() {
    int x = 42;
    int32 y = -100;
}

// ==== case: prim_int32_normal ====
// @expect: accept
void main() {
    int a = 10;
    int b = 20;
    int sum = a + b;
    int diff = a - b;
}

// ==== case: prim_int32_bad ====
// @expect: reject
void main() {
    int x = 5;
    int y = x.length();
}

// ==== case: prim_int64_good ====
// @expect: accept
void main() {
    int64 x = 10000000000;
}

// ==== case: prim_int64_normal ====
// @expect: accept
void main() {
    int64 big = 9223372036854775807;
    int64 half = big / 2;
}

// ==== case: prim_int64_bad ====
// @expect: reject
void main() {
    int64 x = null;
}

// ==== case: prim_uint8_good ====
// @expect: accept
void main() {
    uint8 x = 255;
}

// ==== case: prim_uint8_normal ====
// @expect: accept
void main() {
    uint8 a = 200;
    uint8 b = 50;
    uint8 c = a - b;
}

// ==== case: prim_uint8_bad ====
// @expect: reject
void main() {
    uint8 x = 1;
    x.foo();
}

// ==== case: prim_uint16_good ====
// @expect: accept
void main() {
    uint16 x = 65535;
}

// ==== case: prim_uint16_normal ====
// @expect: accept
void main() {
    uint16 a = 40000;
    uint16 b = 1000;
    uint16 c = a + b;
}

// ==== case: prim_uint16_bad ====
// @expect: reject
void main() {
    uint16@ p;
}

// ==== case: prim_uint32_good ====
// @expect: accept
void main() {
    uint x = 100;
    uint32 y = 200;
}

// ==== case: prim_uint32_normal ====
// @expect: accept
void main() {
    uint a = 10;
    uint b = 20;
    uint c = (a & b) | 1;
}

// ==== case: prim_uint32_bad ====
// @expect: reject
void main() {
    uint@ p;
}

// ==== case: prim_uint64_good ====
// @expect: accept
void main() {
    uint64 x = 10000000000;
}

// ==== case: prim_uint64_normal ====
// @expect: accept
void main() {
    uint64 a = 18446744073709551615;
    uint64 b = a >> 1;
}

// ==== case: prim_uint64_bad ====
// @expect: reject
void main() {
    uint64@ p;
}

// ==== case: prim_float_good ====
// @expect: accept
void main() {
    float x = 3.14f;
}

// ==== case: prim_float_normal ====
// @expect: accept
void main() {
    float a = 1.5f;
    float b = 2.5f;
    float c = a * b + 0.5f;
}

// ==== case: prim_float_bad ====
// @expect: reject
void main() {
    float a = 1.0f;
    float b = a << 1;
}

// ==== case: prim_double_good ====
// @expect: accept
void main() {
    double x = 3.141592653589793;
}

// ==== case: prim_double_normal ====
// @expect: accept
void main() {
    double a = 1.0;
    double b = 2.0;
    double c = (a + b) / 3.0;
}

// ==== case: prim_double_bad ====
// @expect: reject
void main() {
    double a = 2.0;
    double b = a >> 1;
}

// ==== case: handle_null_good ====
// @expect: accept
class Node {}
void main() {
    Node@ n = null;
}

// ==== case: handle_null_normal ====
// @expect: accept
class Node {
    Node@ next;
}
void main() {
    Node@ a = Node();
    @a.next = null;
    if (a.next is null) {}
}

// ==== case: handle_null_bad ====
// @expect: reject
void main() {
    int x = null;
}

// ==== case: handle_decl_assign_good ====
// @expect: accept
class Obj {}
void main() {
    Obj a;
    Obj@ b = a;
}

// ==== case: handle_decl_assign_normal ====
// @expect: accept
class Obj {
    int val;
}
void main() {
    Obj a;
    a.val = 10;
    Obj@ b = @a;
    @b = @a;
    b.val = 20;
}

// ==== case: handle_decl_assign_bad ====
// @expect: reject
void main() {
    int@ p;
}

// ==== case: handle_identity_good ====
// @expect: accept
class Obj {}
void main() {
    Obj@ a = null;
    Obj@ b = null;
    bool same = (a is b);
}

// ==== case: handle_identity_normal ====
// @expect: accept
class Obj {}
void main() {
    Obj@ a = Obj();
    Obj@ b = a;
    Obj@ c = Obj();
    bool eq = (a is b);
    bool neq = (a !is c);
}

// ==== case: handle_identity_bad ====
// @expect: reject
class Obj {}
void main() {
    Obj@ a = null;
    int x = 5;
    bool r = (a is x);
}

// ==== case: handle_value_assign_good ====
// @expect: accept
class Val {
    int x;
}
void main() {
    Val a;
    Val@ h = @a;
    h.x = 42;
}

// ==== case: handle_value_assign_normal ====
// @expect: accept
class Val {
    int x;
}
void main() {
    Val a;
    Val b;
    a.x = 1;
    b.x = 2;
    Val@ ha = @a;
    Val@ hb = @b;
    ha = hb;
}

// ==== case: handle_const_obj_good ====
// @expect: accept
class Item {
    int getVal() const { return 1; }
}
void main() {
    const Item@ h = Item();
    int x = h.getVal();
}

// ==== case: handle_const_obj_normal ====
// @expect: accept
class Item {
    int v;
    int getVal() const { return v; }
}
void main() {
    Item item;
    item.v = 5;
    const Item@ h = @item;
    int res = h.getVal();
}

// ==== case: handle_const_obj_bad ====
// @expect: reject
class Item {
    int v;
}
void main() {
    const Item@ h = Item();
    h.v = 10;
}

// ==== case: handle_const_handle_good ====
// @expect: accept
class Item {}
void main() {
    Item a;
    Item@ const h = @a;
}

// ==== case: handle_const_handle_normal ====
// @expect: accept
class Item {
    int v;
}
void main() {
    Item a;
    a.v = 1;
    Item@ const h = @a;
    h.v = 2;
}

// ==== case: handle_const_handle_bad ====
// @expect: reject
class Item {}
void main() {
    Item a;
    Item b;
    Item@ const h = @a;
    @h = @b;
}

// ==== case: str_concat_good ====
// @expect: accept
void main() {
    string s = "hello" + " " + "world";
}

// ==== case: str_concat_normal ====
// @expect: accept
void main() {
    string s = "count: ";
    int n = 5;
    s += n;
    s = s + " items";
}

// ==== case: str_methods_good ====
// @expect: accept
void main() {
    string s = "abc";
    uint len = s.length();
}

// ==== case: str_methods_normal ====
// @expect: accept
void main() {
    string s = "hello world";
    if (!s.isEmpty()) {
        int pos = s.findFirst("world");
        string sub = s.substr(pos, 5);
    }
}

// ==== case: str_index_good ====
// @expect: accept
void main() {
    string s = "abc";
    uint8 c = s[0];
}

// ==== case: str_index_normal ====
// @expect: accept
void main() {
    string s = "hello";
    uint8 first = s[0];
    s[0] = 72;
}

// ==== case: str_compare_good ====
// @expect: accept
void main() {
    string a = "a";
    string b = "b";
    bool eq = (a == b);
}

// ==== case: str_compare_normal ====
// @expect: accept
void main() {
    string a = "apple";
    string b = "banana";
    bool less = (a < b);
    bool neq = (a != b);
}

// ==== case: str_compare_bad ====
// @expect: reject
class Foo {}
void main() {
    string s = "abc";
    Foo f;
    bool r = (s == f);
}

// ==== case: str_split_good ====
// @expect: accept
void main() {
    string s = "a,b,c";
    array<string>@ parts = s.split(",");
}

// ==== case: str_split_normal ====
// @expect: accept
void main() {
    string s = "one:two:three";
    array<string>@ items = s.split(":");
    for (uint i = 0; i < items.length(); i++) {
        string val = items[i];
    }
}

// ==== case: str_handle_disallowed_good ====
// @expect: accept
void main() {
    string s = "value";
    string copy = s;
}

// ==== case: str_handle_disallowed_normal ====
// @expect: accept
void takeStr(const string &in str) {}
void main() {
    string s = "hello";
    takeStr(s);
}

// ==== case: arr_template_good ====
// @expect: accept
void main() {
    array<int> a;
}

// ==== case: arr_template_normal ====
// @expect: accept
void main() {
    array<string> names;
    names.insertLast("alice");
    names.insertLast("bob");
    uint n = names.length();
}

// ==== case: arr_shorthand_good ====
// @expect: accept
void main() {
    int[] a;
}

// ==== case: arr_shorthand_normal ====
// @expect: accept
void main() {
    string[] words = {"foo", "bar"};
    int[][] matrix = {{1, 2}, {3, 4}};
}

// ==== case: arr_shorthand_bad ====
// @expect: reject
void main() {
    int a[10];
}

// ==== case: arr_init_list_good ====
// @expect: accept
void main() {
    array<int> a = {1, 2, 3};
}

// ==== case: arr_init_list_normal ====
// @expect: accept
void main() {
    array<float> vals = {1.0f, 2.5f, 3.75f};
    array<array<int>> table = {{1, 2}, {3, 4}};
}

// ==== case: arr_init_list_bad ====
// @expect: reject
void main() {
    array<int> a = {1, "bad", 3};
}

// ==== case: arr_indexing_good ====
// @expect: accept
void main() {
    array<int> a = {10, 20};
    int x = a[0];
}

// ==== case: arr_indexing_normal ====
// @expect: accept
void main() {
    array<int> a = {1, 2, 3};
    a[1] = 42;
    int last = a[a.length() - 1];
}

// ==== case: arr_methods_good ====
// @expect: accept
void main() {
    array<int> a = {3, 1, 2};
    a.sortAsc();
}

// ==== case: arr_methods_normal ====
// @expect: accept
void main() {
    array<int> a = {5, 3, 1, 4, 2};
    a.sortDesc();
    a.reverse();
    int idx = a.find(3);
}

// ==== case: arr_handle_good ====
// @expect: accept
void main() {
    array<int>@ a = null;
}

// ==== case: arr_handle_normal ====
// @expect: accept
void main() {
    array<int> a = {1, 2};
    array<int>@ h = @a;
    h.insertLast(3);
}

// ==== case: arr_handle_bad ====
// @expect: reject
void main() {
    array<int>@ a = 5;
}

// ==== case: auto_primitive_good ====
// @expect: accept
void main() {
    auto x = 42;
}

// ==== case: auto_primitive_normal ====
// @expect: accept
void main() {
    auto i = 10;
    auto f = 3.14f;
    auto b = true;
    auto res = i + 5;
}

// ==== case: auto_primitive_bad ====
// @expect: reject
void main() {
    auto x;
}

// ==== case: auto_handle_good ====
// @expect: accept
class Obj {}
void main() {
    Obj o;
    auto@ h = @o;
}

// ==== case: auto_handle_normal ====
// @expect: accept
class Item {
    int id;
}
void main() {
    Item item;
    item.id = 1;
    auto h = item;
    auto@ itemRef = @item;
}

// ==== case: auto_func_return_good ====
// @expect: accept
int getNum() {
    return 100;
}
void main() {
    auto n = getNum();
}

// ==== case: auto_func_return_normal ====
// @expect: accept
array<string> getList() {
    array<string> a;
    a.insertLast("test");
    return a;
}
void main() {
    auto list = getList();
    string s = list[0];
}

// ==== case: auto_func_return_bad ====
// @expect: reject
void doNothing() {}
void main() {
    auto x = doNothing();
}

// ==== case: auto_param_disallowed_good ====
// @expect: accept
void f(int x) {}
void main() {
    auto a = 1;
    f(a);
}

// ==== case: auto_param_disallowed_normal ====
// @expect: accept
int add(int a, int b) {
    return a + b;
}
void main() {
    auto x = 5;
    auto y = 10;
    auto sum = add(x, y);
}

// ==== case: auto_param_disallowed_bad ====
// @expect: reject
void f(auto x) {}
void main() {}

// ==== case: conv_prim_implicit_good ====
// @expect: accept
void main() {
    int a = 10;
    float b = a;
}

// ==== case: conv_prim_implicit_normal ====
// @expect: accept
void main() {
    int8 i8 = 5;
    int32 i32 = i8;
    int64 i64 = i32;
    double d = i64;
}

// ==== case: conv_prim_implicit_bad ====
// @expect: reject
void main() {
    string s = "123";
    int x = s;
}

// ==== case: conv_prim_explicit_good ====
// @expect: accept
void main() {
    float f = 3.99f;
    int i = int(f);
}

// ==== case: conv_prim_explicit_normal ====
// @expect: accept
void main() {
    double d = 123.456;
    uint8 u8 = uint8(d);
    int64 i64 = int64(u8);
    float f = float(i64);
}

// ==== case: conv_handle_cast_good ====
// @expect: accept
class Base {}
class Derived : Base {}
void main() {
    Base@ b = Derived();
    Derived@ d = cast<Derived>(b);
}

// ==== case: conv_handle_cast_normal ====
// @expect: accept
class Base {}
class Derived : Base {
    int val;
}
void main() {
    Base@ b = Derived();
    Derived@ d = cast<Derived>(b);
    if (d !is null) {
        d.val = 42;
    }
}

// ==== case: conv_handle_cast_bad ====
// @expect: reject
class Base {}
void main() {
    Base@ b = Base();
    int@ p = cast<int>(b);
}

// ==== case: conv_op_conv_good ====
// @expect: accept
class IntBox {
    int val;
    int opConv() const { return val; }
}
void main() {
    IntBox b;
    b.val = 10;
    int x = int(b);
}

// ==== case: conv_op_conv_normal ====
// @expect: accept
class IntBox {
    int val;
    int opConv() const { return val; }
    double opConv() const { return double(val); }
}
void main() {
    IntBox b;
    b.val = 20;
    int i = int(b);
    double d = double(b);
}

// ==== case: conv_op_impl_conv_good ====
// @expect: accept
class Num {
    int v;
    int opImplConv() const { return v; }
}
void main() {
    Num n;
    n.v = 7;
    int x = n;
}

// ==== case: conv_op_impl_conv_normal ====
// @expect: accept
class Num {
    int v;
    int opImplConv() const { return v; }
}
void takesInt(int x) {}
void main() {
    Num n;
    n.v = 42;
    takesInt(n);
    int sum = n + 10;
}

// ==== case: conv_op_impl_conv_bad ====
// @expect: reject
class NoImpl {
    int v;
}
void main() {
    NoImpl n;
    int x = n;
}

// ==== case: conv_constructor_good ====
// @expect: accept
class Box {
    int v;
    Box() { v = 0; }
    Box(int a) { v = a; }
}
void main() {
    Box b = 42;
}

// ==== case: conv_constructor_normal ====
// @expect: accept
class Box {
    int v;
    Box() { v = 0; }
    Box(int a) { v = a; }
}
void takeBox(Box b) {}
void main() {
    takeBox(100);
    Box b(200);
}

// ==== case: conv_constructor_bad ====
// @expect: reject
class Box {
    int v;
    Box() { v = 0; }
    explicit Box(int a) { v = a; }
}
void main() {
    Box b = 42;
}

// ==== case: lit_int_formats_good ====
// @expect: accept
void main() {
    int d = 42;
    int h = 0x2A;
    int o = 0o52;
    int b = 0b101010;
}

// ==== case: lit_int_formats_normal ====
// @expect: accept
void main() {
    uint u = 100;
    int64 l = 200;
    uint64 ul = 300;
    int hex = 0xFF;
    int bin = 0b11110000;
}

// ==== case: lit_int_formats_bad ====
// @expect: reject
// manual-vs-compiler: invalid binary digit in 0b literal
void main() {
    int b = 0b102;
}

// ==== case: lit_float_formats_good ====
// @expect: accept
void main() {
    float f = 1.5f;
    double d = 2.5;
}

// ==== case: lit_float_formats_normal ====
// @expect: accept
void main() {
    float f1 = 1e-3f;
    float f2 = .5f;
    double d1 = 1e10;
    double d2 = 3.14159;
}

// ==== case: lit_float_formats_bad ====
// @expect: reject
// manual-vs-compiler: invalid duplicate suffix ff on float literal
void main() {
    float f = 1.0ff;
}

// ==== case: lit_bool_keywords_good ====
// @expect: accept
void main() {
    bool t = true;
    bool f = false;
}

// ==== case: lit_bool_keywords_normal ====
// @expect: accept
void main() {
    bool a = true;
    bool b = !a;
    bool c = (a == false);
}

// ==== case: lit_bool_keywords_bad ====
// @expect: reject
void main() {
    bool b = TRUE;
}

// ==== case: lit_string_escapes_good ====
// @expect: accept
void main() {
    string s = "hello\nworld";
}

// ==== case: lit_string_escapes_normal ====
// @expect: accept
void main() {
    string s1 = "line1\nline2\ttab\"quote\"\\backslash";
    string s2 = 'single \'quote\' string';
}

// ==== case: lit_string_escapes_bad ====
// @expect: reject
void main() {
    string s = "unclosed;
}

// ==== case: lit_heredoc_good ====
// @expect: accept
void main() {
    string s = """heredoc""";
}

// ==== case: lit_heredoc_normal ====
// @expect: accept
void main() {
    string s = """line 1
line 2
"embedded quotes" and 'single'""";
}

// ==== case: lit_heredoc_bad ====
// @expect: reject
void main() {
    string s = """unclosed heredoc;
}

// ==== case: lit_null_keyword_good ====
// @expect: accept
class Obj {}
void main() {
    Obj@ o = null;
}

// ==== case: lit_null_keyword_normal ====
// @expect: accept
class Node {
    Node@ next;
}
void main() {
    Node@ head = Node();
    @head.next = null;
    bool isNull = (head.next is null);
}

// ==== case: lit_null_keyword_bad ====
// @expect: reject
void main() {
    int x = null + 5;
}

// ==== case: lit_char_single_quote_good ====
// @expect: accept
// manual-vs-compiler: single quotes produce string unless asEP_USE_CHARACTER_LITERALS is enabled
void main() {
    string s = 'a';
}

// ==== case: lit_char_single_quote_normal ====
// @expect: accept
void main() {
    string s = 'multi character single quote';
    uint len = s.length();
}

// ==== case: lit_char_single_quote_bad ====
// @expect: reject
// manual-vs-compiler: single quotes produce string in default configuration so assigning to int is rejected
void main() {
    int c = 'a';
}

// ==== case: prec_arith_rel_good ====
// @expect: accept
void main() {
    bool r = 1 + 2 < 3 * 4;
}

// ==== case: prec_arith_rel_normal ====
// @expect: accept
void main() {
    int a = 5;
    int b = 10;
    int c = 2;
    int d = 4;
    bool r = a + b * c > d - b / c;
}

// ==== case: prec_arith_rel_bad ====
// @expect: reject
void main() {
    int x = 5 + (1 < 2 < 3);
}

// ==== case: prec_rel_logical_good ====
// @expect: accept
void main() {
    bool r = 1 < 2 && 3 > 2;
}

// ==== case: prec_rel_logical_normal ====
// @expect: accept
void main() {
    int x = 5;
    bool inRange = x >= 0 && x <= 10 || x == 100;
}

// ==== case: prec_rel_logical_bad ====
// @expect: reject
void main() {
    int x = 5;
    bool r = 1 < x < 10;
}

// ==== case: prec_logical_and_or_xor_good ====
// @expect: accept
void main() {
    bool r = false || true && false;
}

// ==== case: prec_logical_and_or_xor_normal ====
// @expect: accept
void main() {
    bool a = true;
    bool b = false;
    bool c = true;
    bool r = a || b && c ^^ false;
}

// ==== case: prec_logical_and_or_xor_bad ====
// @expect: reject
void main() {
    int a = 1;
    int b = 2;
    int c = a && b;
}

// ==== case: prec_bitwise_rel_good ====
// @expect: accept
// manual-vs-compiler: in AngelScript bitwise operators have higher precedence than comparison operators
void main() {
    bool r = 1 & 2 == 0;
}

// ==== case: prec_bitwise_rel_normal ====
// @expect: accept
void main() {
    int mask = 0xFF;
    int val = 0x0F;
    bool match = (val & mask) != 0 && (val | mask) > 0;
}

// ==== case: prec_bitwise_rel_bad ====
// @expect: reject
void main() {
    string s = "a";
    int x = s & 1;
}

// ==== case: prec_shift_arith_good ====
// @expect: accept
// manual-vs-compiler: addition has higher precedence than shift operators in AngelScript
void main() {
    int x = 1 + 2 << 3;
}

// ==== case: prec_shift_arith_normal ====
// @expect: accept
void main() {
    int a = 2;
    int b = 3;
    int c = 1;
    int r = a * 2 + b << c + 1;
}

// ==== case: prec_shift_arith_bad ====
// @expect: reject
void main() {
    float f = 1.0f;
    float r = f << 2;
}

// ==== case: prec_exponent_good ====
// @expect: accept
// manual-vs-compiler: exponent ** has higher precedence than multiplication *
void main() {
    double x = 2 ** 3 * 4;
}

// ==== case: prec_exponent_normal ====
// @expect: accept
void main() {
    double a = 2.0;
    double b = 3.0;
    double c = 2.0;
    double r = -a ** b + c;
}

// ==== case: prec_exponent_bad ====
// @expect: reject
void main() {
    string s = "a";
    string r = s ** 2;
}

// ==== case: prec_ternary_good ====
// @expect: accept
void main() {
    int x = true ? 1 : 2;
}

// ==== case: prec_ternary_normal ====
// @expect: accept
void main() {
    int a = 10;
    int b = 20;
    int min = a < b ? a : b;
    int status = a > 0 ? (b > 0 ? 1 : 2) : 3;
}

// ==== case: prec_ternary_bad ====
// @expect: reject
class Cat {}
class Dog {}
void main() {
    bool cond = true;
    Cat c;
    Dog d;
    auto r = cond ? c : d;
}

// ==== case: prec_unary_post_good ====
// @expect: accept
void main() {
    int x = 5;
    int y = -x++;
}

// ==== case: prec_unary_post_normal ====
// @expect: accept
class Counter {
    int c;
}
void main() {
    Counter cnt;
    cnt.c = 10;
    int a = -cnt.c++;
    int b = ~a;
    bool neg = !(b > 0);
}

// ==== case: prec_identity_logical_good ====
// @expect: accept
class Obj {}
void main() {
    Obj@ a = null;
    bool b = a is null && true;
}

// ==== case: prec_identity_logical_normal ====
// @expect: accept
class Node {
    Node@ next;
}
void main() {
    Node@ a = Node();
    Node@ b = Node();
    bool ok = a !is null && a.next is b;
}

// ==== case: prec_identity_logical_bad ====
// @expect: reject
class Obj {}
void main() {
    Obj@ a = null;
    Obj@ b = null;
    bool c = a is b is null;
}

// ==== case: auto_handle_bad ====
// @expect: reject
class Foo {
    auto x = 10;
}
void main() {}


// ==== case: conv_op_conv_bad ====
// @expect: reject
class NoConv {}
void main() {
    NoConv c;
    int x = int(c);
}


// ==== case: prec_unary_post_bad ====
// @expect: reject
void main() {
    int x = (1 + 2)++;
}


