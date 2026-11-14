// @expect: accept
// @complete+ QuillDraw
// @complete+ QuillWrite
class Parchment {
    void QuillDraw(int strokes) {}
    void QuillWrite(int strokes) {}
}
void draw(Parchment& p) {
    p.Qu/*$complete*/(5);
}
