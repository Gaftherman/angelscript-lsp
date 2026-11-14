// @expect: accept
// @complete+ Clamp
namespace One { class Clamp {} }
namespace Two { class Clamp {} }
void f() { One::Cl/*$complete*/ x; }
