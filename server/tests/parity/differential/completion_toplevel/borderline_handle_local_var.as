// @expect: accept
// @complete+ MossPatch
// @complete+ MossRock
// @complete- MossFern
class MossPatch {}
class MossRock {}
namespace Bog { class MossFern {} }
void examine() {
    Mo/*$complete*/@ h = null;
}
