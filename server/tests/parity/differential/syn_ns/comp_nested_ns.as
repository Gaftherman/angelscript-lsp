// @expect: accept
// @complete+ valOne
namespace Outer {
    namespace Inner {
        int valOne = 1;
    }
}
int r = Outer::Inner::val/*$complete*/;
