// @expect: accept
// @complete+ helperA
namespace Util {
    void helperA() {}
}
using namespace Util;
void main() {
    hel/*$complete*/();
}
