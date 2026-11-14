// @expect: reject
namespace Math { int sq(int x) { return x * x; } }
void main() { int y = Physics::sq(4); }
