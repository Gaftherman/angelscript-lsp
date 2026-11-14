// @expect: accept
// @complete+ LarkSong
// @complete+ LarkFeather
class LarkSong {}
class LarkFeather {}
namespace Meadow {
    void sing() {
        ::La/*$complete*/ s;
    }
}
