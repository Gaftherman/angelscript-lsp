// @expect: reject
namespace Outer {
    namespace Inner { void f() {} }
    void g() { f(); }
}
