// @expect: accept
// @complete+ FrostPeak
// @complete+ FrostWind
namespace Realm {
    namespace North {
        namespace Peak {
            class FrostPeak {}
            class FrostWind {}
        }
        class FrostValley {}
    }
}
void trek() {
    Realm::North::Peak::Fr/*$complete*/ x;
}
