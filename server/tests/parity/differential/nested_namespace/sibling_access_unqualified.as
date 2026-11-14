// @expect: reject
namespace NorthTerritory {
    void marchTroops() {}
}
namespace SouthTerritory {
    void patrolBorder() {
        marchTroops();
    }
}
