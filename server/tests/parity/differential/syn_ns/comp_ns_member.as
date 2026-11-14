// @expect: accept
// @complete+ actOne
// @complete+ actTwo
namespace Alpha {
    void actOne() {}
    void actTwo() {}
}
void run() {
    Alpha::act/*$complete*/();
}
