// ==== case: col_bad_class_ns_same_name ====
// @expect: accept
namespace A {}
class A {}

// ==== case: col_bad_dup_func_same_ns ====
// @expect: reject
namespace A {
    void f() {}
    void f() {}
}

// ==== case: col_bad_dup_var_same_ns ====
// @expect: reject
namespace A {
    int x = 1;
    int x = 2;
}

// ==== case: col_func_diff_ns ====
// @expect: accept
namespace A { void f(int a) {} }
namespace B { void f(float b) {} }
void main() { A::f(1); B::f(1.0f); }

// ==== case: col_inner_shadows_outer ====
// @expect: accept
namespace A {
    int x = 1;
    namespace B {
        int x = 2;
        int f() { return x; }
    }
}

// ==== case: col_same_name_diff_ns ====
// @expect: accept
namespace A { int x = 1; }
namespace B { int x = 2; }
int y = A::x + B::x;

// ==== case: comp_bad_wrong_ns ====
// @expect: accept
// @complete+ onlyA
// @complete- onlyB
namespace A { void onlyA() {} }
namespace B { void onlyB() {} }
void f() {
    A::only/*$complete*/();
}

// ==== case: comp_global_prefix ====
// @expect: accept
// @complete+ topAlpha
void topAlpha() {}
namespace N {
    void topAlpha() {}
    void f() {
        ::top/*$complete*/();
    }
}

// ==== case: comp_nested_ns ====
// @expect: accept
// @complete+ valOne
namespace Outer {
    namespace Inner {
        int valOne = 1;
    }
}
int r = Outer::Inner::val/*$complete*/;

// ==== case: comp_ns_member ====
// @expect: accept
// @complete+ actOne
// @complete+ actTwo
namespace Alpha {
    void actOne() {}
    void actTwo() {}
}
void run() {
    Alpha::act/*$complete*/();
}

// ==== case: comp_using_ns ====
// @expect: accept
// @complete+ helperA
namespace Util {
    void helperA() {}
}
using namespace Util;
void main() {
    hel/*$complete*/();
}

// ==== case: imp_bad_body ====
// @expect: reject
import void f() from "m" {}

// ==== case: imp_bad_missing_from ====
// @expect: reject
import void f();

// ==== case: imp_bad_missing_module ====
// @expect: reject
import void f() from ;

// ==== case: imp_bad_missing_semi ====
// @expect: reject
// @range: loose
import void f() from "m"

// ==== case: imp_basic ====
// @expect: accept
import void hostFunc(int a) from "mod";
void main() {}

// ==== case: imp_call ====
// @expect: accept
import void ext() from "m";
void main() { ext(); }

// ==== case: imp_ns ====
// @expect: accept
namespace Net {
    import int sendData(int x) from "net";
}
void main() {}

// ==== case: imp_ret_handle ====
// @expect: accept
class Obj {}
import Obj@ createObj() from "factory";

// ==== case: ns_ancestor_lookup ====
// @expect: accept
namespace Outer {
    int baseVal = 5;
    namespace Inner {
        int get() { return baseVal; }
    }
}

// ==== case: ns_bad_empty_double_colon ====
// @expect: reject
namespace A:: {
    void f() {}
}

// ==== case: ns_bad_local_namespace ====
// @expect: reject
void f() {
    namespace Local { int x = 1; }
}

// ==== case: ns_bad_outer_unqualified ====
// @expect: reject
namespace Outer {
    namespace Inner { void f() {} }
    void g() { f(); }
}

// ==== case: ns_bad_sibling_unqualified ====
// @expect: reject
namespace A { void f() {} }
namespace B { void g() { f(); } }

// ==== case: ns_compound_decl ====
// @expect: accept
namespace A::B::C {
    void ping() {}
}
void main() { A::B::C::ping(); }

// ==== case: ns_deep_qualified ====
// @expect: accept
namespace A { namespace B { class Item { int id; } } }
void useItem() { A::B::Item it; it.id = 3; }

// ==== case: ns_global_prefix ====
// @expect: accept
void topFn() {}
namespace A {
    void topFn() {}
    void test() { ::topFn(); }
}

// ==== case: ns_nested_block ====
// @expect: accept
namespace A {
    namespace B {
        int getVal() { return 1; }
    }
}
int x = A::B::getVal();

// ==== case: using_bad_class_name ====
// @expect: accept
class MyCls {}
using namespace MyCls;

// ==== case: using_bad_inside_func ====
// @expect: accept
namespace A { void foo() {} }
void main() {
    using namespace A;
    foo();
}

// ==== case: using_bad_missing_semi ====
// @expect: reject
// @range: loose
namespace A { void f() {} }
using namespace A
void main() {}

// ==== case: using_before_decl ====
// @expect: accept
using namespace A;
namespace A { void foo() {} }
void main() { foo(); }

// ==== case: using_inside_ns ====
// @expect: accept
namespace A { void foo() {} }
namespace C {
    using namespace A;
    void bar() { foo(); }
}

// ==== case: using_multiple ====
// @expect: accept
namespace A { void fa() {} }
namespace B { void fb() {} }
using namespace A;
using namespace B;
void main() { fa(); fb(); }

// ==== case: using_nested_ns ====
// @expect: accept
namespace A::B { int num() { return 7; } }
using namespace A::B;
int x = num();

// ==== case: using_top_level ====
// @expect: accept
namespace A { void foo() {} }
using namespace A;
void main() { foo(); }

// ==== case: ns_bad_wrong_qual_path ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(3, 12, "No matching symbol 'A::C::f'")]
// @expect: reject
namespace A { namespace B { void f() {} } }
void g() { A::C::f(); }

// ==== case: using_bad_ambiguous ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(6, 15, "Multiple matching signatures to 'foo()'")]
// @expect: reject
namespace A { void foo() {} }
namespace B { void foo() {} }
using namespace A;
using namespace B;
void main() { foo(); }
