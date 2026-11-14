// @expect: reject
// @complete- DriftWood
// @complete- DriftCurrent
namespace River {
    class DriftWood {}
    class DriftCurrent {}
}
void useDrift(Dr/*$complete*/ obj) {}
