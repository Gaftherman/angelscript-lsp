// @expect: accept
// @complete+ Quill
// @complete+ QuillPen
// @complete- QuillInk
class Quill {}
class QuillPen {}
namespace Desk { class QuillInk {} }
void writeNote() {
    Qu/*$complete*/ note;
}
