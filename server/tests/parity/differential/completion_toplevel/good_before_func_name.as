// @expect: accept
// @complete+ Brook
// @complete- BrookHidden
class Brook {}
namespace Secret { class BrookHidden {} }
Br/*$complete*/ getBrook() {
    Brook b;
    return b;
}
