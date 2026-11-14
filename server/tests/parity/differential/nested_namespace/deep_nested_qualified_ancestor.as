// @expect: accept
class GlobalBeacon {}
namespace Cosmos {
    class StarCluster {}
    namespace Galaxy {
        namespace Solar {
            void observeStars() {
                ::GlobalBeacon b;
                Cosmos::StarCluster s;
            }
        }
    }
}
