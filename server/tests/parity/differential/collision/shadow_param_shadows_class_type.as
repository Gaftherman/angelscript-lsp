// @expect: accept
// @complete+ MossLichen
// @complete- SporeCell
namespace Meadow {
    class SporeCell {}
}
namespace Swamp {
    class MossLichen {}
    void cultivate(int MossLichen) {
        Swamp::Mo/*$complete*/ m;
    }
}
