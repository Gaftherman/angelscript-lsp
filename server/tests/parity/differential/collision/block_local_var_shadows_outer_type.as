// @expect: accept
// @complete+ TimberWood
// @complete- CedarPine
class TimberWood {}
namespace Alpine {
    class CedarPine {}
}
void harvest() {
    {
        int TimberWood = 99;
    }
    Ti/*$complete*/ t;
}
