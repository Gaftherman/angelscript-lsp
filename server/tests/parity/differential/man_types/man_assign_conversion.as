// ==== case: prop_set_void_bad ====
// @predefined: sdk-addons
// @expect: reject
void voidFn() {}
class Player {
    private string _p;
    string get_p() const property { return _p; }
    void set_p(const string&in v) property { _p = v; }
}
void main() {
    Player obj;
    obj.p = voidFn();
}


// ==== case: prop_set_float_bad ====
// @predefined: sdk-addons
// @expect: reject
float floatFn() { return 1.5f; }
class Player {
    private string _p;
    string get_p() const property { return _p; }
    void set_p(const string&in v) property { _p = v; }
}
void main() {
    Player obj;
    obj.p = floatFn();
}


// ==== case: prop_set_funcdef_handle_bad ====
// @predefined: sdk-addons
// @expect: reject
funcdef void Cb();
Cb@ funcdefHandleFn() { return null; }
class Player {
    private string _p;
    string get_p() const property { return _p; }
    void set_p(const string&in v) property { _p = v; }
}
void main() {
    Player obj;
    obj.p = funcdefHandleFn();
}


// ==== case: prop_set_string_good ====
// @predefined: sdk-addons
// @expect: accept
class Player {
    private string _p;
    string get_p() const property { return _p; }
    void set_p(const string&in v) property { _p = v; }
}
void main() {
    Player obj;
    obj.p = "ok";
}


// ==== case: prop_write_only_bad ====
// @predefined: sdk-addons
// @expect: reject
class PropHost {
    void set_w(const string&in v) property {}
}
void main() {
    PropHost h;
    h.w = 123;
}


// ==== case: prop_write_only_good ====
// @predefined: sdk-addons
// @expect: accept
class PropHost {
    void set_w(const string&in v) property {}
}
void main() {
    PropHost h;
    h.w = "ok";
}


// ==== case: prop_inherited_bad ====
// @predefined: sdk-addons
// @expect: reject
class Base {
    int get_bp() const property { return 0; }
    void set_bp(int v) property {}
}
class Sub : Base {}
void main() {
    Sub s;
    s.bp = "bad";
}


// ==== case: prop_inherited_good ====
// @expect: accept
class Base {
    int get_bp() const property { return 0; }
    void set_bp(int v) property {}
}
class Sub : Base {}
void main() {
    Sub s;
    s.bp = 42;
}


// ==== case: prop_global_void_bad ====
// @expect: reject
void voidFn() {}
int get_gp() property { return 0; }
void set_gp(int v) property {}
void main() {
    gp = voidFn();
}


// ==== case: prop_indexed_bad ====
// @predefined: sdk-addons
// @expect: reject
float floatFn() { return 1.5f; }
class Grid {
    string get_cell(int i) const property { return ""; }
    void set_cell(int i, const string&in v) property {}
}
void main() {
    Grid g;
    g.cell[0] = floatFn();
}


// ==== case: prop_indexed_good ====
// @predefined: sdk-addons
// @expect: accept
class Grid {
    string get_cell(int i) const property { return ""; }
    void set_cell(int i, const string&in v) property {}
}
void main() {
    Grid g;
    g.cell[0] = "ok";
}


// ==== case: prop_readonly_bad ====
// @expect: reject
class ReadOnlyHost {
    int get_ro() const property { return 1; }
}
void main() {
    ReadOnlyHost h;
    h.ro = 2;
}


// ==== case: prop_opassign_class_bad ====
// @expect: reject
class Box {
    Box& opAssign(float v) { return this; }
}
class Host {
    void set_box(const Box&in b) property {}
}
void main() {
    Host h;
    h.box = 1.5f;
}


// ==== case: field_string_void_bad ====
// @predefined: sdk-addons
// @expect: reject
void voidFn() {}
class Player {
    string name;
}
void main() {
    Player obj;
    obj.name = voidFn();
}


// ==== case: field_string_float_good ====
// @predefined: sdk-addons
// @expect: accept
float floatFn() { return 1.5f; }
class Player {
    string name;
}
void main() {
    Player obj;
    obj.name = floatFn();
}


// ==== case: local_string_void_bad ====
// @predefined: sdk-addons
// @expect: reject
void voidFn() {}
void main() {
    string s;
    s = voidFn();
}


// ==== case: local_string_funcdef_bad ====
// @predefined: sdk-addons
// @expect: reject
funcdef void Cb();
Cb@ funcdefHandleFn() { return null; }
void main() {
    string s;
    s = funcdefHandleFn();
}


// ==== case: global_int_void_bad ====
// @expect: reject
void voidFn() {}
int g_val = 0;
void main() {
    g_val = voidFn();
}


// ==== case: base_field_string_bad ====
// @predefined: sdk-addons
// @expect: reject
class Base {
    int baseVal;
}
class Sub : Base {}
void main() {
    Sub s;
    s.baseVal = "bad";
}


// ==== case: base_field_int_good ====
// @expect: accept
class Base {
    int baseVal;
}
class Sub : Base {}
void main() {
    Sub s;
    s.baseVal = 42;
}


// ==== case: array_elem_void_bad ====
// @predefined: sdk-addons
// @expect: reject
void voidFn() {}
void main() {
    array<int> a = {1, 2};
    a[0] = voidFn();
}


// ==== case: array_elem_string_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    array<int> a = {1, 2};
    a[0] = "bad";
}


// ==== case: array_elem_int_good ====
// @predefined: sdk-addons
// @expect: accept
void main() {
    array<int> a = {1, 2};
    a[0] = 7;
}


// ==== case: array_mismatch_float_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    array<int> a;
    array<float> b;
    a = b;
}


// ==== case: array_assign_scalar_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    array<int> a;
    a = 5;
}


// ==== case: array_match_good ====
// @predefined: sdk-addons
// @expect: accept
void main() {
    array<int> a;
    array<int> b;
    a = b;
}


// ==== case: opassign_str_void_bad ====
// @predefined: sdk-addons
// @expect: reject
void voidFn() {}
class StrBox {
    StrBox& opAssign(const string&in s) { return this; }
}
class Holder {
    StrBox st;
}
void main() {
    Holder h;
    h.st = voidFn();
}


// ==== case: opassign_str_funcdef_bad ====
// @predefined: sdk-addons
// @expect: reject
funcdef void Cb();
Cb@ funcdefHandleFn() { return null; }
class StrBox {
    StrBox& opAssign(const string&in s) { return this; }
}
class Holder {
    StrBox st;
}
void main() {
    Holder h;
    h.st = funcdefHandleFn();
}


// ==== case: opassign_str_unrelated_bad ====
// @predefined: sdk-addons
// @expect: reject
class Unrelated {}
class StrBox {
    StrBox& opAssign(const string&in s) { return this; }
}
class Holder {
    StrBox st;
}
void main() {
    Holder h;
    Unrelated u;
    h.st = u;
}


// ==== case: opassign_float_good ====
// @expect: accept
class FloatBox {
    FloatBox& opAssign(float v) { return this; }
}
void main() {
    FloatBox b;
    b = 1.5f;
    b = 2;
}


// ==== case: opassign_float_str_bad ====
// @predefined: sdk-addons
// @expect: reject
class FloatBox {
    FloatBox& opAssign(float v) { return this; }
}
void main() {
    FloatBox b;
    b = "bad";
}


// ==== case: converting_ctor_good ====
// @expect: accept
class BoxCtor {
    BoxCtor() {}
    BoxCtor(int v) {}
}
void main() {
    BoxCtor b;
    b = 42;
}


// ==== case: converting_ctor_explicit_bad ====
// @expect: reject
class BoxExplicit {
    BoxExplicit() {}
    BoxExplicit(int v) explicit {}
}
void main() {
    BoxExplicit b;
    b = 42;
}


// ==== case: opimplconv_good ====
// @expect: accept
class Src {
    int opImplConv() const { return 1; }
}
void main() {
    int i;
    float f;
    Src s;
    i = s;
    f = s;
}


// ==== case: opconv_explicit_bad ====
// @expect: reject
class Src {
    int opConv() const { return 1; }
}
void main() {
    int i;
    Src s;
    i = s;
}


// ==== case: funcdef_handle_good ====
// @expect: accept
funcdef void CbA();
funcdef void CbA2();
void fnA() {}
CbA@ fnAHandle() { return @fnA; }
void main() {
    CbA@ cb;
    @cb = @fnA;
    @cb = fnA;
    @cb = fnAHandle();
    @cb = null;
    CbA2@ cb2;
    @cb2 = cb;
}


// ==== case: funcdef_handle_mismatch_bad ====
// @expect: reject
funcdef void CbA();
funcdef int CbB(int);
int fnB(int x) { return x; }
CbB@ fnBHandle() { return @fnB; }
void main() {
    CbA@ cb;
    @cb = fnBHandle();
}


// ==== case: funcdef_value_to_int_bad ====
// @expect: reject
void fnA() {}
void main() {
    int x = 0;
    x = fnA;
}


// ==== case: funcdef_addr_to_int_bad ====
// @expect: reject
void fnA() {}
void main() {
    int x = 0;
    x = @fnA;
}


// ==== case: other_class_handle_bad ====
// @expect: reject
class A {}
class B {}
void main() {
    A@ a;
    B@ b = B();
    @a = @b;
}


// ==== case: derived_to_base_handle_good ====
// @expect: accept
class Base {}
class Sub : Base {}
void main() {
    Base@ b;
    Sub@ s = Sub();
    @b = @s;
}


// ==== case: null_to_handle_good ====
// @expect: accept
class Item {}
void main() {
    Item@ it;
    @it = null;
}


// ==== case: null_to_int_bad ====
// @expect: reject
void main() {
    int x = 0;
    x = null;
}


// ==== case: enum_to_int_good ====
// @expect: accept
enum Color { Red, Green }
void main() {
    int x = 0;
    float f = 0.0f;
    x = Color::Red;
    f = Color::Green;
}


// ==== case: int_to_enum_bad ====
// @expect: reject
enum Color { Red, Green }
void main() {
    Color c = Color::Red;
    c = 1;
}


// ==== case: ternary_mixed_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    int x = 0;
    x = true ? 1 : "bad";
}


// ==== case: ternary_numeric_good ====
// @expect: accept
void main() {
    float f = 0.0f;
    f = true ? 1 : 2.5f;
}


// ==== case: chained_call_void_bad ====
// @predefined: sdk-addons
// @expect: reject
class Inner {
    void getVoid() {}
}
class Outer {
    Inner getInner() { return Inner(); }
}
Outer getOuter() { return Outer(); }
void main() {
    string s;
    s = getOuter().getInner().getVoid();
}


// ==== case: chained_call_method_good ====
// @predefined: sdk-addons
// @expect: accept
class Inner {
    string getName() { return "ok"; }
}
class Outer {
    Inner getInner() { return Inner(); }
}
Outer getOuter() { return Outer(); }
void main() {
    string s;
    s = getOuter().getInner().getName();
}


// ==== case: compound_opaddassign_good ====
// @expect: accept
class Vec {
    Vec& opAddAssign(float v) { return this; }
}
void main() {
    Vec v;
    v += 1.5f;
    v += 2;
}


// ==== case: compound_opaddassign_bad ====
// @predefined: sdk-addons
// @expect: reject
class Vec {
    Vec& opAddAssign(float v) { return this; }
}
void main() {
    Vec v;
    v += "bad";
}


// ==== case: compound_no_opaddassign_bad ====
// @expect: reject
class Plain {}
void main() {
    Plain a;
    Plain b;
    a += b;
}


// ==== case: compound_primitive_void_bad ====
// @expect: reject
void voidFn() {}
void main() {
    int x = 1;
    x += voidFn();
}


// ==== case: compound_enum_bad ====
// @expect: reject
enum Color { Red, Green }
void main() {
    Color c = Color::Red;
    c += Color::Red;
}


// ==== case: compound_bitwise_float_bad ====
// @expect: reject
void main() {
    int x = 1;
    x &= 1.5f;
}


// ==== case: multiline_prop_void_bad ====
// @predefined: sdk-addons
// @expect: reject
void voidFn() {}
class Player {
    string get_p() const property { return ""; }
    void set_p(const string&in v) property {}
}
void main() {
    Player obj;
    obj.p =

        voidFn();
}


// ==== case: multiline_local_void_bad ====
// @expect: reject
void voidFn() {}
void main() {
    float f = 0.0f;
    f =

        voidFn();
}


// ==== case: typedef_target_good ====
// @expect: accept
typedef int MyInt;
void main() {
    MyInt x = 0;
    x = 42;
}


// ==== case: typedef_target_bad ====
// @predefined: sdk-addons
// @expect: reject
typedef int MyInt;
void main() {
    MyInt x = 0;
    x = "bad";
}


// ==== case: typedef_target_void_bad ====
// @expect: reject
typedef int MyInt;
void voidFn() {}
void main() {
    MyInt x = 0;
    x = voidFn();
}
