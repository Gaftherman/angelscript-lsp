// @expect: reject
// @complete- EmberGlow
class FireWood {
    int LogWeight;
}
class CoalMine {
    int EmberGlow;
}
void warm(FireWood& f) {
    int x = f.Em/*$complete*/;
}
