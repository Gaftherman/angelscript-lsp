// @expect: reject
// @complete- BrookTrout
namespace Valley {
    namespace East {
        class BrookTrout {}
    }
    namespace West {
        void fish() {
            Br/*$complete*/ b;
        }
    }
}
