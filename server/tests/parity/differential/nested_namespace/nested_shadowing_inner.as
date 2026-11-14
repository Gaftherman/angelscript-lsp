// @expect: accept
namespace Forest {
    class LeafFoliage { int outerVal; }
    namespace Glade {
        class LeafFoliage { int innerVal; }
        void checkLeaves() {
            LeafFoliage l;
            l.innerVal = 1;
        }
    }
}
