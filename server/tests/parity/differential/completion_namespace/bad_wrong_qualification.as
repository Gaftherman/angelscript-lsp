// @expect: reject
// @complete- QuillHedge
namespace Forest {
    namespace Deep {
        class QuillHedge {}
    }
}
void gather() {
    Deep::Qu/*$complete*/ q;
}
