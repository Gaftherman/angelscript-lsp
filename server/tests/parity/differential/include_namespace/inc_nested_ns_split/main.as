// @expect: accept
// @complete+ HollowCave
// @complete- ThornBush
#include "forest.as"
#include "deep.as"
void explore() {
    Forest::Deep::Ho/*$complete*/ h;
}
