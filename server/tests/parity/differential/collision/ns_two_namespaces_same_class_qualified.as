// @expect: accept
// @complete+ Quill
// @complete- ScrollMark
namespace Feather { class Quill {} }
namespace Scroll { class Quill {} class ScrollMark {} }
void writeDoc() {
    Feather::Qu/*$complete*/ q;
}
