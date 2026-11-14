// @expect: accept
// @complete+ PalmLeaf
// @complete+ PalmTree
namespace Oasis {
    class PalmTree {}
    namespace Inner {
        class PalmLeaf {}
        void harvest() {
            Pa/*$complete*/ x;
        }
    }
}
