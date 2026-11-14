// @expect: accept
// @complete+ BloomColor
// @complete+ BloomScent
// @complete- BloomSecret
class FlowerBase {
    int BloomColor;
    int BloomScent;
    private int BloomSecret;
}
class Orchid : FlowerBase {}
void inspect(Orchid& o) {
    int val = o.Bl/*$complete*/;
}
