// @expect: accept
// @complete+ River
namespace Valley { class Pebble {} }
namespace River {
    class River {}
    void drift() {
        Ri/*$complete*/ r;
    }
}
