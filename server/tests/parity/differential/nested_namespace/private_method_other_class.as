// @expect: reject
namespace GardenCourtyard {
    class RoseBush {
        private void bloomFlower() {}
    }
    class Caretaker {
        void tendGarden() {
            RoseBush bush;
            bush.bloomFlower();
        }
    }
}
