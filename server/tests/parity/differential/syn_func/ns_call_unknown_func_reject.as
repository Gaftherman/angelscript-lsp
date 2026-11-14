// @expect: reject
namespace Math { int sq(int x) { return x * x; } }
void main() { int y = Math::cube(4); }
