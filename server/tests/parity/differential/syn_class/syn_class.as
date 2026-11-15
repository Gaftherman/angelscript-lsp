// ==== case: abstract_class_subclass ====
// @expect: accept
abstract class Shape {
    int area() const { return 0; }
}
class Square : Shape {
    int area() const override { return 4; }
}
void main() {
    Square s;
    Shape@ p = s;
}

// ==== case: abstract_instantiate_err ====
// @expect: reject
abstract class Shape {}
void main() {
    Shape s;
}

// ==== case: copy_ctor_and_opAssign ====
// @expect: accept
class Item {
    int v;
    Item() { v = 0; }
    Item(const Item &in other) { v = other.v; }
    Item &opAssign(const Item &in other) {
        v = other.v;
        return this;
    }
}
void main() {
    Item a;
    a.v = 9;
    Item b(a);
    Item c;
    c = b;
}

// ==== case: ctor_default_and_param ====
// @expect: accept
class Vec {
    int x;
    Vec() { x = 0; }
    Vec(int a) { x = a; }
}
void main() {
    Vec a;
    Vec b(5);
}

// ==== case: ctor_delete_default ====
// @expect: reject
class NoDefault {
    NoDefault() = delete;
    NoDefault(int x) {}
}
void main() {
    NoDefault a(1);
}

// ==== case: ctor_delete_default_call_err ====
// @expect: reject
class NoDefault {
    NoDefault() = delete;
    NoDefault(int x) {}
}
void main() {
    NoDefault a;
}

// ==== case: dtor_basic ====
// @expect: accept
class Res {
    ~Res() {}
}

// ==== case: dtor_with_param_err ====
// @expect: reject
class Res {
    ~Res(int a) {}
}

// ==== case: dtor_with_return_type_err ====
// @expect: reject
class Res {
    void ~Res() {}
}

// ==== case: external_class_err ====
// @expect: reject
external class ExtObj;

// ==== case: inherit_from_final_err ====
// @expect: reject
final class Base {}
class Derived : Base {}

// ==== case: inherit_multiple_classes_err ====
// @expect: reject
class A {}
class B {}
class C : A, B {}

// ==== case: inherit_single ====
// @expect: accept
class Base {
    int x;
}
class Derived : Base {
    int y;
}
void main() {
    Derived d;
    d.x = 1;
    d.y = 2;
}

// ==== case: inherit_undefined_base_err ====
// @expect: reject
class Derived : MissingBase {}

// ==== case: interface_basic ====
// @expect: accept
interface IFoo {
    int getVal() const;
}
class Foo : IFoo {
    int getVal() const { return 42; }
}

// ==== case: interface_inherit_interface ====
// @expect: accept
interface IA { void a(); }
interface IB : IA { void b(); }
class C : IB {
    void a() {}
    void b() {}
}

// ==== case: interface_method_body_err ====
// @expect: reject
interface IFoo {
    void a() {}
}

// ==== case: interface_missing_method_err ====
// @expect: reject
interface IFoo {
    void a();
    void b();
}
class Foo : IFoo {
    void a() {}
}

// ==== case: interface_multiple ====
// @expect: accept
interface IA { void a(); }
interface IB { void b(); }
class C : IA, IB {
    void a() {}
    void b() {}
}

// ==== case: interface_with_field_err ====
// @expect: reject
interface IFoo {
    int x;
}

// ==== case: missing_default_ctor_err ====
// @expect: reject
class OnlyParam {
    OnlyParam(int x) {}
}
void main() {
    OnlyParam a;
}

// ==== case: mixin_basic ====
// @expect: accept
mixin class MyMixin {
    int m;
    int getM() const { return m; }
}
class Host : MyMixin {
    void test() { m = 5; }
}

// ==== case: mixin_inherit_err ====
// @expect: reject
class Base {}
mixin class MyMixin : Base {}
class C : MyMixin {}

// ==== case: mixin_instantiate_err ====
// @expect: reject
mixin class MyMixin {
    int m;
}
void main() {
    MyMixin x;
}

// ==== case: op_add ====
// @expect: accept
class Num {
    int v;
    Num(int x = 0) { v = x; }
    Num opAdd(const Num &in rhs) const {
        return Num(v + rhs.v);
    }
}
void main() {
    Num a(1), b(2);
    Num c = a + b;
}

// ==== case: op_call ====
// @expect: accept
class Fn {
    int opCall(int x) const { return x + 1; }
}
void main() {
    Fn f;
    int r = f(10);
}

// ==== case: op_equals_cmp ====
// @expect: accept
class Num {
    int v;
    Num(int x = 0) { v = x; }
    bool opEquals(const Num &in rhs) const { return v == rhs.v; }
    int opCmp(const Num &in rhs) const { return v - rhs.v; }
}
void main() {
    Num a(1), b(2);
    bool eq = (a == b);
    bool lt = (a < b);
}

// ==== case: op_index ====
// @expect: accept
class Arr {
    int opIndex(int i) const { return i * 2; }
}
void main() {
    Arr a;
    int x = a[3];
}

// ==== case: op_neg_preinc ====
// @expect: accept
class Counter {
    int v;
    Counter(int x = 0) { v = x; }
    Counter opNeg() const { return Counter(-v); }
    Counter &opPreInc() { ++v; return this; }
}
void main() {
    Counter c(5);
    Counter n = -c;
    ++c;
}

// ==== case: override_method ====
// @expect: accept
class Base {
    int f() { return 1; }
}
class Derived : Base {
    int f() override { return 2; }
}

// ==== case: private_access_derived_err ====
// @expect: reject
class Base {
    private int code;
}
class Derived : Base {
    int read() const { return code; }
}

// ==== case: private_access_external_err ====
// @expect: reject
class Secret {
    private int code;
}
void main() {
    Secret s;
    s.code = 1;
}

// ==== case: private_access_internal ====
// @expect: accept
class Secret {
    private int code;
    void setCode(int c) { code = c; }
    int getCode() const { return code; }
}

// ==== case: property_get_set ====
// @expect: accept
class Box {
    private int val;
    int value {
        get const { return val; }
        set { val = value; }
    }
}
void main() {
    Box b;
    b.value = 10;
    int x = b.value;
}

// ==== case: property_read_writeonly_err ====
// @expect: reject
class Box {
    private int val;
    int value {
        set { val = value; }
    }
}
void main() {
    Box b;
    int x = b.value;
}

// ==== case: property_readonly ====
// @expect: accept
class Box {
    int value {
        get const { return 7; }
    }
}
void main() {
    Box b;
    int x = b.value;
}

// ==== case: property_write_readonly_err ====
// @expect: reject
class Box {
    int value {
        get const { return 7; }
    }
}
void main() {
    Box b;
    b.value = 5;
}

// ==== case: property_writeonly ====
// @expect: accept
class Box {
    private int val;
    int value {
        set { val = value; }
    }
}
void main() {
    Box b;
    b.value = 3;
}

// ==== case: protected_access_derived ====
// @expect: accept
class Base {
    protected int code;
}
class Derived : Base {
    int read() const { return code; }
}

// ==== case: protected_access_external_err ====
// @expect: reject
class Base {
    protected int code;
}
void main() {
    Base b;
    b.code = 1;
}

// ==== case: shared_class ====
// @expect: accept
shared class SharedObj {
    int id;
    int getId() const { return id; }
}

// ==== case: super_ctor_call ====
// @expect: accept
class Base {
    int x;
    Base(int a) { x = a; }
}
class Derived : Base {
    Derived(int a) {
        super(a);
    }
}

// ==== case: super_outside_ctor_err ====
// @expect: reject
class Base {
    Base(int a) {}
}
class Derived : Base {
    Derived() { super(1); }
    void f() { super(2); }
}

// ==== case: final_abstract_conflict_err ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(2, 7, 'A class cannot be both abstract and final')]
// @expect: reject
final abstract class Bad {}

// ==== case: inherit_circular_err ====
// @expect: reject
class A : B {}
class B : A {}

// ==== case: override_final_method_err ====
// @expect: reject
class Base {
    int f() final { return 1; }
}
class Derived : Base {
    int f() override { return 2; }
}

// ==== case: override_missing_base_err ====
// @expect: reject
class Base {}
class Derived : Base {
    int f() override { return 2; }
}
