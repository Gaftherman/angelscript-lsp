// @expect: accept
// @complete+ BeaconTower
// @complete- LanternPost
#include "east.as"
#include "west.as"
void signal() {
    East::Be/*$complete*/ b;
}
