// @expect: accept
// @complete+ CrystalGlow
// @complete+ CrystalShard
namespace Cavern {
    class CrystalGlow {}
    class CrystalShard {}
}
class CrystalCavern {}
void mine() {
    Cavern::Cr/*$complete*/ c;
}
