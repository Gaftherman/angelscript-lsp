// @expect: reject
namespace SkyDome {
    void soarHigh() {}
    namespace CloudCover {
        void makeRain() {
            SkyDome::thunderStrike();
        }
    }
}
