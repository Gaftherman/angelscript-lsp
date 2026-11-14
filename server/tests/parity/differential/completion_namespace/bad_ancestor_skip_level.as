// @expect: reject
// @complete- StonePillar
namespace Alpha {
    namespace Beta {
        namespace Gamma {
            class StonePillar {}
        }
    }
}
void build() {
    Alpha::Gamma::St/*$complete*/ s;
}
