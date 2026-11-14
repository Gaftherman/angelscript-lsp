// @expect: accept
// @complete+ BrookBridge
namespace East {
    class BrookBridge {}
}
namespace West {
    class BrookBridge {}
}
void cross() {
    East::Br/*$complete*/ b;
}
