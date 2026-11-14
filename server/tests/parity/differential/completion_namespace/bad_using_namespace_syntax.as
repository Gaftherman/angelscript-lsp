// @expect: reject
namespace Glen {
    class FernSpore {}
}
using namespace Glen;
void inspect() {
    Fe/*$complete*/ x;
}
