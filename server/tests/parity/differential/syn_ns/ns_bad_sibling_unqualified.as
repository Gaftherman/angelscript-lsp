// @expect: reject
namespace A { void f() {} }
namespace B { void g() { f(); } }
