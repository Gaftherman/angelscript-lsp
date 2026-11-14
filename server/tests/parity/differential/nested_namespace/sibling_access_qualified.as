// @expect: accept
namespace Valley {
    void streamWater() {}
}
namespace Hill {
    void lookoutPoint() {
        Valley::streamWater();
    }
}
