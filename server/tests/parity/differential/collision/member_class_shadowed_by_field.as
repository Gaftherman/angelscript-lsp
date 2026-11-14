// @expect: accept
// @complete+ CanyonRock
// @complete- BoulderStone
namespace Ridge {
    class BoulderStone {}
}
class CanyonRock {}
class Gorge {
    int CanyonRock;
    void explore() {
        ::Ca/*$complete*/ c;
    }
}
