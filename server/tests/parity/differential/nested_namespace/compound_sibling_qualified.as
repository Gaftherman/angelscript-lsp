// @expect: accept
namespace Realm::Forest {
    class OakTree {}
}
namespace Realm::Mountain {
    void gazeTrees() {
        Realm::Forest::OakTree tree;
    }
}
