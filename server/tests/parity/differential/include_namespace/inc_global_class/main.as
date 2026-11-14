// @expect: accept
// @complete+ MeadowGrass
// @complete- DesertDune
#include "decl.as"
void graze() {
    Me/*$complete*/ m;
}
