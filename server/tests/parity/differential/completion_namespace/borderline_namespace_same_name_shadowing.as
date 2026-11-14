// @expect: accept
// @complete+ Heath
namespace Moor {
    class Heath {
        int outerVal;
    }
    namespace Deep {
        class Heath {
            int innerVal;
        }
        void check() {
            He/*$complete*/ h;
            int v = h.innerVal;
        }
    }
}
