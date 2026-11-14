// @expect: accept
namespace A { void fa() {} }
namespace B { void fb() {} }
using namespace A;
using namespace B;
void main() { fa(); fb(); }
