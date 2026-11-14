// @expect: accept
namespace Outer {
    int baseVal = 5;
    namespace Inner {
        int get() { return baseVal; }
    }
}
