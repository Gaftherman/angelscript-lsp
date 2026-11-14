// @expect: accept
// @complete+ CliffRock
// @complete+ CliffEdge
namespace Ridge {
    class CliffRock {}
    class CliffEdge {}
}
class CliffFallen {}
void climb() {
    Ridge::Cl/*$complete*/ x;
}
