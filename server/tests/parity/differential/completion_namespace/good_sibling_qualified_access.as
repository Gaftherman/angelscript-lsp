// @expect: accept
// @complete+ BrookTrout
// @complete+ BrookSalmon
namespace Valley {
    namespace East {
        class BrookTrout {}
        class BrookSalmon {}
    }
    namespace West {
        void fish() {
            East::Br/*$complete*/ b;
        }
    }
}
