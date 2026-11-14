// @expect: accept
namespace Math { int sq(int x) { return x * x; } }
void main() { int y = Math::sq(4); }
