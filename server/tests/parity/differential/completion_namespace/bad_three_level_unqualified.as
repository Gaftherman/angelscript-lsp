// @expect: reject
// @complete- EmberCore
namespace Core {
    namespace Engine {
        namespace Sub {
            class EmberCore {}
        }
    }
}
void run() {
    Em/*$complete*/ x;
}
