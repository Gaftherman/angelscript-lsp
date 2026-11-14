// @expect: accept
// @complete+ Cloud
// @complete- Hidden
#include "inc.as"
void f() { Cl/*$complete*/ x; }
