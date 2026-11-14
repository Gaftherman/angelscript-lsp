// @expect: accept
// @complete+ Haven
namespace Haven {
    class Cedar {}
}
namespace Forest {
    class Timber {}
}
namespace Sanctuary {
    class Haven {}
    void rest() {
        Ha/*$complete*/ h;
    }
}
